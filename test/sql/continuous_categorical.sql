\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- provsql.categorical(probs[], values[]) builds the same structural
-- representation the Dirac-mixture-collapse simplifier produces: a
-- fresh gate_input key plus one gate_mulinput per outcome, all wrapped
-- in a categorical-form gate_mixture (wires [key, mul_1, ..., mul_n]).
-- The Expectation / RangeCheck / AnalyticEvaluator handlers all
-- dispatch on isCategoricalMixture, so a categorical()-built RV
-- behaves identically to a Dirac-collapsed mixture.

-- A.  Basic shape: the root is a gate_mixture with N+1 wires; the
--     first wire is a gate_input key; the rest are gate_mulinputs.
CREATE TEMP TABLE cat_a AS
  SELECT random_variable_uuid(
           provsql.categorical(ARRAY[0.3, 0.7]::float8[],
                               ARRAY[0,   10]::float8[])) AS u;

SELECT get_gate_type(u)                              AS root_kind,
       array_length(get_children(u), 1)              AS nb_children,
       get_gate_type((get_children(u))[1])           AS wire0_kind,
       get_gate_type((get_children(u))[2])           AS wire1_kind,
       get_gate_type((get_children(u))[3])           AS wire2_kind
  FROM cat_a;

-- All mulinputs share the same key gate (wires[0]).
SELECT (get_children(u))[1] = (get_children((get_children(u))[2]))[1] AS mul1_key_matches,
       (get_children(u))[1] = (get_children((get_children(u))[3]))[1] AS mul2_key_matches
  FROM cat_a;

-- Per-mulinput probability and value match what was passed in.
SELECT abs(get_prob((get_children(u))[2]) - 0.3)  < 1e-12 AS mul1_prob,
       abs(get_prob((get_children(u))[3]) - 0.7)  < 1e-12 AS mul2_prob,
       get_extra((get_children(u))[2])                    AS mul1_value,
       get_extra((get_children(u))[3])                    AS mul2_value
  FROM cat_a;

-- B.  Moments match the analytic categorical:
--       E[X]   = 0.3·0 + 0.7·10 = 7
--       Var[X] = 0.3·0² + 0.7·10² - 7² = 21
SELECT abs(provsql.rv_moment((SELECT u FROM cat_a), 1, false) - 7.0)  < 1e-12 AS cat_mean,
       abs(provsql.rv_moment((SELECT u FROM cat_a), 2, true)  - 21.0) < 1e-12 AS cat_variance;

-- C.  Closed-form cmp probability via AnalyticEvaluator.  Rejecting MC
--     fallback (rv_mc_samples = 0) confirms the answer is analytic.
SET provsql.rv_mc_samples = 0;

CREATE TEMP TABLE cat_gt AS
  SELECT provsql.rv_cmp_gt(
           provsql.categorical(ARRAY[0.3, 0.7]::float8[],
                               ARRAY[0,   10]::float8[]),
           provsql.as_random(0.5)) AS t;

SELECT abs(provsql.probability_evaluate((SELECT t FROM cat_gt), 'independent', '')
           - 0.7) < 1e-12 AS cat_gt_analytic;

-- C2.  Exact equality on a discrete outcome: P(X = 0) = 0.3.  The
--      AnalyticEvaluator's categoricalDecide handles EQ / NE in the
--      discrete sense (vs RangeCheck's measure-theoretic P(X = c) = 0
--      for continuous RVs).
CREATE TEMP TABLE cat_eq AS
  SELECT provsql.rv_cmp_eq(
           provsql.categorical(ARRAY[0.3, 0.7]::float8[],
                               ARRAY[0,   10]::float8[]),
           provsql.as_random(0)) AS t;

SELECT abs(provsql.probability_evaluate((SELECT t FROM cat_eq), 'independent', '')
           - 0.3) < 1e-12 AS cat_eq_analytic;

RESET provsql.rv_mc_samples;

-- D.  Zero-probability outcomes are skipped: the resulting block has
--     only the positive-mass outcomes.
CREATE TEMP TABLE cat_zero AS
  SELECT random_variable_uuid(
           provsql.categorical(ARRAY[0.4, 0.0, 0.6]::float8[],
                               ARRAY[1,   2,   3]::float8[])) AS u;

SELECT array_length(get_children(u), 1) - 1 AS cat_zero_nb_mulinputs  -- minus the key
  FROM cat_zero;

-- E.  Validation errors.
\set VERBOSITY terse

-- E1. Mismatched array lengths.
SELECT provsql.categorical(ARRAY[0.5, 0.5]::float8[], ARRAY[0, 1, 2]::float8[]);

-- E2. Probabilities don't sum to 1.
SELECT provsql.categorical(ARRAY[0.5, 0.3]::float8[], ARRAY[0, 1]::float8[]);

-- E3. Probability out of [0, 1].
SELECT provsql.categorical(ARRAY[1.5, -0.5]::float8[], ARRAY[0, 1]::float8[]);

-- E4. Non-finite value.
SELECT provsql.categorical(ARRAY[0.5, 0.5]::float8[], ARRAY[0, 'NaN'::float8]);
SELECT provsql.categorical(ARRAY[0.5, 0.5]::float8[], ARRAY[0, 'Infinity'::float8]);

-- E5. Empty arrays.
SELECT provsql.categorical(ARRAY[]::float8[], ARRAY[]::float8[]);

\set VERBOSITY default
