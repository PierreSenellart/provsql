\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The hybrid-evaluator's Dirac-mixture-collapse pass rewrites every
-- gate_mixture whose leaves are all gate_value Diracs into a single
-- categorical block: [key, mul_1, ..., mul_n], each mulinput carrying
-- its outcome probability in set_prob and its value in extra.  The
-- rewrite is in-memory only; the persisted DAG is unchanged.  We
-- introspect via simplified_circuit_subgraph, which now runs the
-- hybrid simplifier so consumers see the same circuit
-- probability_evaluate would.

SET provsql.simplify_on_load     = on;
SET provsql.hybrid_evaluation    = on;

-- A.  Bimodal Dirac mixture(0.3, 0, 10) collapses to a 2-outcome
--     categorical block.  Introspect: count of mulinputs sharing the
--     fresh key, their probabilities (0.3 and 0.7) and extras (0, 10).
CREATE TEMP TABLE bimodal AS
  SELECT random_variable_uuid(
           provsql.mixture(0.3::float8,
                           provsql.as_random(0),
                           provsql.as_random(10))) AS u;

WITH s AS (
  SELECT jsonb_array_elements(
           provsql.simplified_circuit_subgraph((SELECT u FROM bimodal), 4)) AS e
),
muls AS (
  SELECT (e->>'extra')::float8 AS value,
         (e->>'prob')::float8  AS prob
    FROM s
   WHERE (e->>'gate_type') = 'mulinput'
)
SELECT count(*) AS bimodal_nb_mulinputs FROM muls;

WITH s AS (
  SELECT jsonb_array_elements(
           provsql.simplified_circuit_subgraph((SELECT u FROM bimodal), 4)) AS e
),
muls AS (
  SELECT (e->>'extra')::float8 AS value,
         (e->>'prob')::float8  AS prob
    FROM s
   WHERE (e->>'gate_type') = 'mulinput'
)
SELECT abs(SUM(CASE WHEN value = 0  THEN prob END) - 0.3) < 1e-12 AS prob_for_value_0_matches,
       abs(SUM(CASE WHEN value = 10 THEN prob END) - 0.7) < 1e-12 AS prob_for_value_10_matches
  FROM muls;

-- B.  Nested 3-component cascade mixture(0.2, 0,
--                                          mixture(0.5, 1, 2))
--     collapses to a 3-outcome categorical block with weights
--     {0.2, 0.4, 0.4} and extras {0, 1, 2}.
CREATE TEMP TABLE cascade AS
  SELECT random_variable_uuid(
           provsql.mixture(0.2::float8,
             provsql.as_random(0),
             provsql.mixture(0.5::float8,
               provsql.as_random(1),
               provsql.as_random(2)))) AS u;

WITH s AS (
  SELECT jsonb_array_elements(
           provsql.simplified_circuit_subgraph((SELECT u FROM cascade), 4)) AS e
)
SELECT count(*) AS cascade_nb_mulinputs
  FROM s WHERE (e->>'gate_type') = 'mulinput';

WITH s AS (
  SELECT jsonb_array_elements(
           provsql.simplified_circuit_subgraph((SELECT u FROM cascade), 4)) AS e
),
muls AS (
  SELECT (e->>'extra')::float8 AS value,
         (e->>'prob')::float8  AS prob
    FROM s
   WHERE (e->>'gate_type') = 'mulinput'
)
SELECT abs(SUM(CASE WHEN value = 0 THEN prob END) - 0.2) < 1e-12 AS cascade_prob_0,
       abs(SUM(CASE WHEN value = 1 THEN prob END) - 0.4) < 1e-12 AS cascade_prob_1,
       abs(SUM(CASE WHEN value = 2 THEN prob END) - 0.4) < 1e-12 AS cascade_prob_2
  FROM muls;

-- C.  P(X > 0.5) on the bimodal Dirac mixture is exactly 0.7 -- the
--     mulinput at value 10 contributes its full mass, the one at
--     value 0 contributes nothing.  The AnalyticEvaluator decides
--     cmp(categorical, value) closed-form, so disabling the MC
--     fallback (rv_mc_samples = 0) does not stop the computation.
SET provsql.rv_mc_samples = 0;

CREATE TEMP TABLE cmp_gate AS
  SELECT provsql.rv_cmp_gt(
           provsql.random_variable_make((SELECT u FROM bimodal),
                                        'NaN'::float8),
           provsql.as_random(0.5)) AS t;

SELECT abs(provsql.probability_evaluate((SELECT t FROM cmp_gate), 'independent', '')
           - 0.7) < 1e-12 AS analytic_cmp_categorical;

RESET provsql.rv_mc_samples;

-- D.  Negative case: a mixture with one gate_rv leaf does NOT collapse;
--     it stays in classic 3-wire form (no mulinputs introduced).
CREATE TEMP TABLE mixed AS
  SELECT random_variable_uuid(
           provsql.mixture(0.5::float8,
                           provsql.normal(0, 1),
                           provsql.as_random(0))) AS u;

WITH s AS (
  SELECT jsonb_array_elements(
           provsql.simplified_circuit_subgraph((SELECT u FROM mixed), 4)) AS e
)
SELECT count(*) FILTER (WHERE (e->>'gate_type') = 'mulinput')
                                                 AS mixed_nb_mulinputs,
       count(*) FILTER (WHERE (e->>'gate_type') = 'mixture')
                                                 AS mixed_nb_mixtures
  FROM s;

-- E.  rv_moment over the bimodal Dirac mixture matches the analytic
--     mean (0.3·0 + 0.7·10 = 7) and variance
--     (0.3·0 + 0.7·100 - 49 = 21) exactly -- the Expectation evaluator
--     handles the categorical form via its own gate_mixture branch.
SELECT abs(provsql.rv_moment((SELECT u FROM bimodal), 1, false) - 7.0)  < 1e-12 AS bimodal_mean,
       abs(provsql.rv_moment((SELECT u FROM bimodal), 2, true)  - 21.0) < 1e-12 AS bimodal_variance;
