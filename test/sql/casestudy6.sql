\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Case Study 6: The City Air-Quality Sensor Network
-- Tests the random_variable surface end-to-end: gate_rv leaves for the
-- four distribution families, the planner-hook lifting of WHERE
-- comparators on RV columns (gate_cmp), Bernoulli mixtures via
-- provsql.mixture, RV aggregates (sum / avg) lowered through
-- rv_aggregate_semimod, expected() applied to a grouped RV via the
-- subquery + outer-WHERE idiom, UNION ALL on RV columns, and an
-- independent / tree-decomposition / monte-carlo cross-check.  Backs
-- doc/source/user/casestudy6.rst.

-- Pin the MC RNG and sample budget so cases that fall back to MC
-- (probabilities, conditional moments) are reproducible at a tolerance
-- loose enough to absorb sampler noise but tight enough to be
-- meaningful.
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples    = 50000;

-- ---------------------------------------------------------------------
-- Setup: schema and seed, mirroring doc/casestudy6/setup.sql.
-- ---------------------------------------------------------------------
CREATE TABLE stations (
  id        text PRIMARY KEY,
  name      text NOT NULL,
  district  text NOT NULL
);
INSERT INTO stations VALUES
  ('s1', 'City Centre',        'centre'),
  ('s2', 'Riverside Park',     'centre'),
  ('s3', 'Industrial Estate',  'east'),
  ('s4', 'Suburban Reference', 'east');
SELECT add_provenance('stations');

CREATE TABLE calibration_status (
  station_id  text PRIMARY KEY REFERENCES stations(id),
  p           double precision NOT NULL
);
INSERT INTO calibration_status VALUES
  ('s1', 0.95),
  ('s2', 0.70),
  ('s3', 0.60),
  ('s4', 1.00);
SELECT add_provenance('calibration_status');

CREATE TABLE readings (
  id          integer PRIMARY KEY,
  station_id  text NOT NULL REFERENCES stations(id),
  ts          timestamp NOT NULL,
  pm25        random_variable NOT NULL
);
INSERT INTO readings (id, station_id, ts, pm25) VALUES
  (1, 's1', '2026-05-12 08:00', provsql.normal(28.0, 2.0)),
  (2, 's2', '2026-05-12 08:00', provsql.uniform(10.0, 22.0)),
  (3, 's3', '2026-05-12 08:00', provsql.exponential(0.04)),
  (4, 's4', '2026-05-12 08:00', 15.0),
  (5, 's1', '2026-05-12 09:00', provsql.normal(40.0, 4.0)),
  (6, 's2', '2026-05-12 09:00', provsql.uniform(12.0, 24.0)),
  (7, 's3', '2026-05-12 09:00', provsql.erlang(3, 0.1)),
  (8, 's4', '2026-05-12 09:00', 16.5);
SELECT add_provenance('readings');

CREATE TABLE historical_readings (
  id          integer PRIMARY KEY,
  station_id  text NOT NULL REFERENCES stations(id),
  ts          timestamp NOT NULL,
  pm25        random_variable NOT NULL
);
INSERT INTO historical_readings (id, station_id, ts, pm25) VALUES
  (101, 's1', '2026-05-11 08:00', provsql.normal(34.0, 2.5)),
  (102, 's2', '2026-05-11 08:00', provsql.uniform(15.0, 28.0)),
  (103, 's3', '2026-05-11 08:00', provsql.exponential(0.03)),
  (104, 's4', '2026-05-11 08:00', 18.0),
  (105, 's1', '2026-05-11 09:00', provsql.normal(42.0, 3.0)),
  (106, 's2', '2026-05-11 09:00', provsql.uniform(20.0, 35.0)),
  (107, 's3', '2026-05-11 09:00', provsql.erlang(3, 0.08)),
  (108, 's4', '2026-05-11 09:00', 19.5);
SELECT add_provenance('historical_readings');

-- ---------------------------------------------------------------------
-- Step 1: per-row pm25 gates.  Rows 1 and 5 are Normal; row 3 is
-- Exponential; row 4 is the deterministic reference station lifted via
-- the implicit numeric -> random_variable cast (gate_value); row 7 is
-- Erlang.  One representative pick per shape.  The materialise +
-- remove_provenance dance hides the row's own auto-added provsql uuid
-- column from the output (it's non-deterministic).
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_pm25_kind AS
  SELECT id, get_gate_type(pm25::uuid) AS pm25_kind
    FROM readings
   WHERE id IN (1, 3, 4, 7);
SELECT remove_provenance('result_cs6_pm25_kind');
SELECT id, pm25_kind FROM result_cs6_pm25_kind ORDER BY id;
DROP TABLE result_cs6_pm25_kind;

-- ---------------------------------------------------------------------
-- Step 2: WHERE pm25 > 35 lifts the comparator into a gate_cmp
-- conjoined with each row's input via gate_times; the procedure body
-- of random_variable_gt is never executed, so every row survives the
-- WHERE.  The pm25 column kind on rows 5 and 7 confirms that the lift
-- did not strip the underlying distribution.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_thresh AS
  SELECT id, station_id,
         get_gate_type(provenance())  AS prov_kind,
         get_gate_type(pm25::uuid)    AS pm25_kind
    FROM readings
   WHERE pm25 > 35;
SELECT remove_provenance('result_cs6_thresh');
SELECT id, station_id, prov_kind, pm25_kind
  FROM result_cs6_thresh ORDER BY id;
DROP TABLE result_cs6_thresh;

-- Probability that the event pm25 > 35 fires on each tracked row.  For
-- the s1 readings (Normal):
--   row 1, N(28, 2):  P(X > 35) = 1 - Phi( 3.5) ~= 2.3e-4
--   row 5, N(40, 4):  P(X > 35) = 1 - Phi(-1.25) ~= 0.8944
-- The analytic 'independent' path is exact for a single gate_cmp under
-- a deterministic input gate; MC at seed=42 must sit within 0.02 of it.
CREATE TABLE result_cs6_thresh_prob AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'monte-carlo', '50000')
             - probability_evaluate(provenance(), 'independent')) < 0.02
           AS mc_matches_independent
    FROM readings
   WHERE pm25 > 35
     AND station_id = 's1';
SELECT remove_provenance('result_cs6_thresh_prob');
SELECT id, mc_matches_independent
  FROM result_cs6_thresh_prob ORDER BY id;
DROP TABLE result_cs6_thresh_prob;

-- ---------------------------------------------------------------------
-- Step 3: provsql.mixture(p, x, y) returns a gate_mixture; the
-- pm25 / 1.2 arm (the calibration correction) is a gate_arith(DIV)
-- over the per-row pm25 and a gate_value.  The constructor result
-- type is random_variable, so ::uuid grabs the gate root.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_mixture AS
  SELECT r.id,
         get_gate_type(
           provsql.mixture(cs.p, r.pm25, r.pm25 / 1.2)::uuid
         ) AS mixture_kind,
         get_gate_type(
           (r.pm25 / 1.2)::uuid
         ) AS scaled_kind
    FROM readings r JOIN calibration_status cs USING (station_id)
   WHERE r.station_id = 's1';
SELECT remove_provenance('result_cs6_mixture');
SELECT id, mixture_kind, scaled_kind
  FROM result_cs6_mixture ORDER BY id;
DROP TABLE result_cs6_mixture;

-- ---------------------------------------------------------------------
-- Step 4: sum(pm25) and avg(pm25) over a tracked RV column lower
-- through rv_aggregate_semimod into gate_arith roots.  Aggregate
-- dispatch on the random_variable result type picks the provsql.sum /
-- provsql.avg overloads from plain sum() / avg(), so the case study's
-- SQL uses the bare names.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_agg AS
  SELECT s.district,
         sum(r.pm25) AS total_pm25,
         avg(r.pm25) AS avg_pm25
    FROM readings r JOIN stations s ON s.id = r.station_id
   GROUP BY s.district;
SELECT remove_provenance('result_cs6_agg');

-- Structure: sum -> gate_arith(PLUS); avg -> gate_arith(DIV) over two
-- gate_arith(PLUS) subtrees (numerator / denominator).
SELECT district,
       get_gate_type(total_pm25::uuid) AS sum_root,
       get_gate_type(avg_pm25::uuid)   AS avg_root
  FROM result_cs6_agg ORDER BY district;

-- The 'centre' district has rows 1, 2, 5, 6 (N(28,2), U(10,22),
-- N(40,4), U(12,24)) with E[sum] = 28 + 16 + 40 + 18 = 102.
-- The 'east' district has rows 3, 4, 7, 8 (Exp(0.04), 15, Erl(3,0.1),
-- 16.5) with E[sum] = 25 + 15 + 30 + 16.5 = 86.5.  Linearity of
-- expectation hits exactly under the analytical evaluator.
SELECT district,
       abs(provsql.expected(total_pm25) - CASE district
             WHEN 'centre' THEN 102.0
             WHEN 'east'   THEN  86.5
                                              END) < 1e-9
         AS total_mean_exact
  FROM result_cs6_agg ORDER BY district;

DROP TABLE result_cs6_agg;

-- ---------------------------------------------------------------------
-- Step 8: UNION ALL across today's batch and the historical batch
-- preserves both branches' provenance via gate_plus.  The pm25 column
-- is forwarded verbatim, so the per-row gate_rv shapes survive.
-- count(*) is computed after remove_provenance so the agg_token shell
-- doesn't render as "4 (*)" / "12 (*)" in the output.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_union_raw AS
    SELECT get_gate_type(pm25::uuid) AS pm25_kind FROM readings
  UNION ALL
    SELECT get_gate_type(pm25::uuid) AS pm25_kind FROM historical_readings;
SELECT remove_provenance('result_cs6_union_raw');
SELECT pm25_kind, count(*) AS n
  FROM result_cs6_union_raw
  GROUP BY pm25_kind
  ORDER BY pm25_kind;
DROP TABLE result_cs6_union_raw;

-- ---------------------------------------------------------------------
-- Step 9: filter the per-district aggregates by their expected average.
-- The natural form `HAVING expected(avg(r.pm25)) > 20` is not yet
-- supported by the planner hook (it accepts provenance_aggregate /
-- Var / Const as HAVING operands but not arbitrary FuncExprs over
-- aggregates); the working idiom pushes the aggregate through a
-- subquery and filters in the outer WHERE.  E[avg(pm25)] is 102/4 =
-- 25.5 for the centre district and 86.5/4 = 21.625 for east, so both
-- rows should pass the > 20 threshold.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_having AS
  SELECT district
    FROM (
      SELECT s.district, avg(r.pm25) AS avg_pm25
        FROM readings r JOIN stations s ON s.id = r.station_id
       GROUP BY s.district
    ) t
   WHERE expected(avg_pm25) > 20;
SELECT remove_provenance('result_cs6_having');
SELECT district FROM result_cs6_having ORDER BY district;
DROP TABLE result_cs6_having;

-- ---------------------------------------------------------------------
-- Step 10: independent vs monte-carlo on the threshold query.  For
-- each surviving row the closed-form 'independent' result is exact;
-- MC at n=50000 must match within 0.02.  tree-decomposition is also
-- exact and must agree with 'independent' to full precision.
-- ---------------------------------------------------------------------
CREATE TABLE result_cs6_methods AS
  SELECT id,
         abs(probability_evaluate(provenance(), 'independent')
             - probability_evaluate(provenance(), 'tree-decomposition')) < 1e-9
           AS ind_equals_td,
         abs(probability_evaluate(provenance(), 'independent')
             - probability_evaluate(provenance(), 'monte-carlo', '50000')) < 0.02
           AS ind_close_to_mc
    FROM readings WHERE pm25 > 35;
SELECT remove_provenance('result_cs6_methods');
SELECT id, ind_equals_td, ind_close_to_mc
  FROM result_cs6_methods ORDER BY id;
DROP TABLE result_cs6_methods;

-- ---------------------------------------------------------------------
-- Cleanup.
-- ---------------------------------------------------------------------
DROP TABLE historical_readings;
DROP TABLE readings;
DROP TABLE calibration_status;
DROP TABLE stations;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
