\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Seed the MC RNG so the reported probabilities are reproducible
-- across runs.  Each section is structured as a tolerance check
-- (|p_mc - p_truth| < eps) rather than echoing the raw float so the
-- expected output stays stable when the RNG seeding macros change.
SET provsql.monte_carlo_seed = 42;

-- ====================================================================
-- Setup: two-group RV-valued table with an INT weight column.  The
-- RV column drives the WHERE-lifted comparison; the weight drives
-- SUM / MIN / MAX / AVG HAVING aggregates.
--   hot  rows: val ~ N(2, 1) so P(val > 1) = Φ(1) ≈ 0.8413
--   warm rows: val ~ U(0, 2) so P(val > 1) = 0.5
-- Two rows per group; weight = 5 throughout.
-- ====================================================================
CREATE TABLE readings_w(category text, val random_variable, weight int);
INSERT INTO readings_w VALUES
  ('hot',  provsql.normal(2, 1),  5),
  ('hot',  provsql.normal(2, 1),  5),
  ('warm', provsql.uniform(0, 2), 5),
  ('warm', provsql.uniform(0, 2), 5);
SELECT add_provenance('readings_w');

-- ====================================================================
-- A: COUNT(*) > 1 -- the canonical HAVING+RV test (section G of
-- continuous_selection.sql was structural-only because the gate_agg
-- arm in monteCarloRV::evalScalar did not exist).
--   hot:  P(both rows pass) = 0.8413^2 ≈ 0.7079
--   warm: P(both rows pass) = 0.5^2    = 0.25
-- ====================================================================
CREATE TABLE result_a AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.7079
                             WHEN 'warm' THEN 0.25 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING count(*) > 1;
SELECT remove_provenance('result_a');
SELECT * FROM result_a ORDER BY category;
DROP TABLE result_a;

-- ====================================================================
-- B: SUM(weight) > 4.  Weight is 5 per row; SUM over passing rows in
-- a 2-row group is 0 / 5 / 10.  The HAVING is true on 5 or 10.
--   hot:  P(SUM>4)  = 1 - (1-0.8413)^2 ≈ 0.9748
--   warm: P(SUM>4)  = 1 - 0.5^2        = 0.75
-- ====================================================================
CREATE TABLE result_b AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.9748
                             WHEN 'warm' THEN 0.75 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING sum(weight) > 4;
SELECT remove_provenance('result_b');
SELECT * FROM result_b ORDER BY category;
DROP TABLE result_b;

-- ====================================================================
-- C: SUM(weight) > 7 -- forces BOTH rows to pass (single row sums to
-- 5 only).
--   hot:  0.8413^2 ≈ 0.7079
--   warm: 0.5^2    = 0.25
-- ====================================================================
CREATE TABLE result_c AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.7079
                             WHEN 'warm' THEN 0.25 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING sum(weight) > 7;
SELECT remove_provenance('result_c');
SELECT * FROM result_c ORDER BY category;
DROP TABLE result_c;

-- ====================================================================
-- D: MAX(weight) > 4.  MAX over an empty subset is NaN (no row to
-- pick) and any cmp against NaN is false; MAX over a non-empty subset
-- is just 5 here (all weights equal), so the cmp fires iff at least
-- one row passes.
--   hot:  1 - (1-0.8413)^2 ≈ 0.9748
--   warm: 1 - 0.5^2        = 0.75
-- ====================================================================
CREATE TABLE result_d AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.9748
                             WHEN 'warm' THEN 0.75 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING max(weight) > 4;
SELECT remove_provenance('result_d');
SELECT * FROM result_d ORDER BY category;
DROP TABLE result_d;

-- ====================================================================
-- E: MIN(weight) > 4.  Same as MAX here since all weights are 5.
--   hot:  0.9748
--   warm: 0.75
-- ====================================================================
CREATE TABLE result_e AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.9748
                             WHEN 'warm' THEN 0.75 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING min(weight) > 4;
SELECT remove_provenance('result_e');
SELECT * FROM result_e ORDER BY category;
DROP TABLE result_e;

-- ====================================================================
-- F: AVG(weight) > 4.  AVG over the passing rows; equal weights make
-- the average 5 whenever any row passes, NaN on empty subset.
--   hot:  0.9748
--   warm: 0.75
-- ====================================================================
CREATE TABLE result_f AS
  SELECT category,
         abs(probability_evaluate(provenance(), 'monte-carlo', '100000')
             - CASE category WHEN 'hot'  THEN 0.9748
                             WHEN 'warm' THEN 0.75 END) < 0.02
         AS within_tolerance
    FROM readings_w WHERE val > 1
    GROUP BY category HAVING avg(weight) > 4;
SELECT remove_provenance('result_f');
SELECT * FROM result_f ORDER BY category;
DROP TABLE result_f;

DROP TABLE readings_w;
RESET provsql.monte_carlo_seed;
