\set ECHO none
\pset format unaligned

-- ==========================================================================
-- gate_case: a general CASE over random variables.  A searched CASE whose
-- WHEN guards are RV comparisons and whose THEN/ELSE branches are random
-- variables is lowered (in the SELECT target list) into a gate_case guarded
-- selection [guard_1, value_1, ..., default], first-match semantics.  This
-- subsumes abs / clamp / ReLU as piecewise sugar.  MC-backed here (seed
-- pinned); the closed-form partition integration is deferred.
-- ==========================================================================

SET provsql.active = on;
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 200000;
SET search_path TO public, provsql;

CREATE TABLE d AS SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z;

-- max via CASE equals greatest: E = 3/4.  variance of the two-way max
-- (CASE WHEN x>=y THEN x ELSE y) is Var(Beta(2,1)) = 2/36 = 0.0556.
SELECT abs(expected(CASE WHEN x>=y AND x>=z THEN x
                         WHEN y>=z          THEN y
                         ELSE z END) - 0.75) < 0.02          AS case_max_ok,
       abs(variance(CASE WHEN x>=y THEN x ELSE y END) - 2.0/36) < 0.02
                                                             AS case_var_ok
  FROM d;

-- abs as CASE: E|N(0,1)| = sqrt(2/pi) ~ 0.79788.
SELECT abs(expected(CASE WHEN n>=0 THEN n ELSE -n END) - sqrt(2.0/pi())) < 0.02
       AS abs_ok
FROM (SELECT normal(0,1) AS n) t;

-- ReLU as CASE: E[max(N,0)] = 1/sqrt(2*pi) ~ 0.39894.
SELECT abs(expected(CASE WHEN n>=0 THEN n ELSE as_random(0) END)
           - 1.0/sqrt(2*pi())) < 0.02 AS relu_ok
FROM (SELECT normal(0,1) AS n) t;

-- Closed-form guard-partition integration: with the Monte Carlo fallback
-- disabled (rv_mc_samples = 0) the single-pivot (abs / ReLU / clamp, guards vs
-- a constant, arms affine in the pivot), two-arm (a two-RV comparison guard,
-- each arm affine in an operand), and order-statistic (a first-match tournament
-- that computes max / min of k RVs) shapes are all evaluated exactly, not
-- sampled.
SET provsql.rv_mc_samples = 0;
-- abs: E = sqrt(2/pi), Var = 1 - 2/pi (single Normal pivot, arms n / -n).
SELECT abs(expected(CASE WHEN n>=0 THEN n ELSE -n END) - sqrt(2.0/pi())) < 1e-9
         AS abs_exact,
       abs(variance(CASE WHEN n>=0 THEN n ELSE -n END) - (1 - 2.0/pi())) < 1e-9
         AS abs_var_exact
  FROM (SELECT normal(0,1) AS n) t;
-- ReLU: E = 1/sqrt(2*pi) (pivot n, arms n / constant 0).
SELECT abs(expected(CASE WHEN n>=0 THEN n ELSE as_random(0) END)
           - 1.0/sqrt(2*pi())) < 1e-9 AS relu_exact
  FROM (SELECT normal(0,1) AS n) t;
-- clamp Uniform(0,1) to [0.25, 0.75]: E = 0.5 by symmetry (multi-threshold).
SELECT abs(expected(CASE WHEN x < 0.25 THEN as_random(0.25)
                         WHEN x > 0.75 THEN as_random(0.75)
                         ELSE x END) - 0.5) < 1e-9 AS clamp_exact
  FROM (SELECT uniform(0,1) AS x) t;
-- two-way max: E[max(x,y)] = 2/3, Var = Var(Beta(2,1)) = 1/18 (two distinct
-- uniform operands; each arm is affine in one of them).
SELECT abs(expected(CASE WHEN x>=y THEN x ELSE y END) - 2.0/3) < 1e-9 AS max_exact,
       abs(variance(CASE WHEN x>=y THEN x ELSE y END) - 1.0/18) < 1e-9 AS max_var_exact
  FROM (SELECT uniform(0,1) x, uniform(0,1) y) t;
-- two-way min via swapped arms: E[min(x,y)] = 1/3 (pivot is the arm's operand,
-- the other integrated out to a CDF weight).
SELECT abs(expected(CASE WHEN x>=y THEN y ELSE x END) - 1.0/3) < 1e-9 AS min_exact
  FROM (SELECT uniform(0,1) x, uniform(0,1) y) t;
-- three-way max as a first-match tournament, recognised as an order statistic:
-- max(x,y,z) of i.i.d. U(0,1) ~ Beta(3,1): E = 3/4, Var = 3/80.
SELECT abs(expected(CASE WHEN x>=y AND x>=z THEN x
                         WHEN y>=z          THEN y
                         ELSE z END) - 0.75) < 1e-9 AS threeway_max_exact,
       abs(variance(CASE WHEN x>=y AND x>=z THEN x
                         WHEN y>=z          THEN y
                         ELSE z END) - 3.0/80) < 1e-9 AS threeway_max_var_exact
  FROM (SELECT uniform(0,1) x, uniform(0,1) y, uniform(0,1) z) t;
-- three-way min tournament: min(x,y,z) of i.i.d. U(0,1) ~ Beta(1,3): E = 1/4.
SELECT abs(expected(CASE WHEN x<=y AND x<=z THEN x
                         WHEN y<=z          THEN y
                         ELSE z END) - 0.25) < 1e-9 AS threeway_min_exact
  FROM (SELECT uniform(0,1) x, uniform(0,1) y, uniform(0,1) z) t;
SET provsql.rv_mc_samples = 200000;

-- The lowered value is a first-class random_variable backed by a gate_case;
-- its support is the union of the branch supports (exact, no MC).  A CASE has
-- to be materialised for a set-returning consumer (support / rv_sample), the
-- same pattern the aggregates use, since a multi-output FROM function cannot
-- coexist with an engaged provenance gate.
CREATE TABLE case_mat AS SELECT CASE WHEN x>=y THEN x ELSE y END AS m FROM d;
SELECT get_gate_type(m::uuid) = 'case' AS is_gate_case FROM case_mat;
SELECT s.lo = 0 AS case_lo_0, s.hi = 1 AS case_hi_1
  FROM case_mat, support(m) s;
-- The set-returning RV consumers (rv_sample / rv_histogram) also accept a
-- gate_case root (it is a scalar the sampler evaluates).
SELECT count(*) = 20 AS case_sample_ok FROM case_mat, rv_sample(m::uuid, 20) s;
SELECT jsonb_array_length(rv_histogram(m::uuid, 5)) = 5 AS case_histogram_ok
  FROM case_mat;

-- A general semiring refuses a gate_case (a guarded selection is not a
-- semiring operation), exactly as it refuses gate_rv / gate_conditioned.
DO $$
DECLARE
  tok uuid := (SELECT m::uuid FROM case_mat);
BEGIN
  PERFORM provsql.sr_counting(tok);
  RAISE NOTICE 'case_semiring_refused=%', false;
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'case_semiring_refused=%', true;
END
$$;

DROP TABLE case_mat;
DROP TABLE d;

SELECT 'ok'::text AS continuous_case_done;
