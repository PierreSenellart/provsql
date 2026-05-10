\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- RangeCheck: support-based interval arithmetic decides gate_cmps
-- whose two sides have provably ordered or disjoint supports.  When
-- it decides, the cmp is replaced by a Bernoulli gate_input carrying
-- the certain probability (0 or 1), so the result is exact and no
-- Monte Carlo sampling is performed.
--
-- These tests verify the decided cases.  The (provably 1.0 / 0.0)
-- assertions below would not hold if MC ran on top, since MC has
-- finite-sample noise; we therefore use exact equality (=) rather
-- than abs(... - truth) < tol.

-- (1) U(1, 2) > 0  -- support [1, 2] is strictly above 0, so P = 1.0.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.uniform(1, 2), 0::random_variable),
         'monte-carlo', '1000000') = 1.0
       AS uniform_above_zero_decided;

-- (2) -U(1, 2) > 0  -- support [-2, -1] is strictly at-or-below 0,
-- so P = 0.0.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(- provsql.uniform(1, 2), 0::random_variable),
         'monte-carlo', '1000000') = 0.0
       AS uniform_negated_decided;

-- (3) Exponential is supported on [0, +inf), so Exp(1) >= 0 is always
-- true.  Decided exactly without sampling.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_ge(provsql.exponential(1), 0::random_variable),
         'monte-carlo', '1000000') = 1.0
       AS exponential_nonnegative_decided;

-- (4) U(1, 2) <= 0 -- the upper bound 2 is not <= 0, but every U
-- value is in [1, 2] which is strictly above 0; P(<=0) = 0.0.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(provsql.uniform(1, 2), 0::random_variable),
         'monte-carlo', '1000000') = 0.0
       AS uniform_le_zero_decided;

-- (5) Composition: gate_arith propagates the support through PLUS.
-- U(1, 2) + U(1, 2) ranges over [2, 4], so > 1 is always true.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.uniform(1, 2) + provsql.uniform(1, 2),
                           1::random_variable),
         'monte-carlo', '1000000') = 1.0
       AS arith_plus_above_decided;

-- (6) NEG of an exponential: -Exp(1) is supported on (-inf, 0], so
-- > 0 is always false.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(- provsql.exponential(1), 0::random_variable),
         'monte-carlo', '1000000') = 0.0
       AS arith_neg_exp_decided;

-- (7) Inconclusive case: N(0, 1) > 0 has analytical answer 0.5, but
-- the normal distribution has unbounded support so RangeCheck cannot
-- decide.  This must fall through to MC -- demonstrated by the
-- result NOT being exact (abs(p - 0.5) is non-zero with very high
-- probability for finite samples).  We assert it is within tolerance,
-- which is the underlying sampler invariant.  An analytic-CDF pass
-- could decide this case exactly when one is added.
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1), 0::random_variable),
             'monte-carlo', '100000') - 0.5) < 0.01
       AS normal_undecided_falls_through_to_mc;
RESET provsql.monte_carlo_seed;

-- (8) End-to-end through the planner hook: WHERE rv > c on a
-- provenance-tracked table where the cmp is decided by RangeCheck.
-- Result: every row's provsql is gate_one (input gate with p=1), and
-- probability_evaluate returns 1.0 exactly for every row.
CREATE TABLE rc_sensors(id text, reading provsql.random_variable);
INSERT INTO rc_sensors VALUES
  ('a', provsql.uniform(5, 10)),     -- > 0 always
  ('b', provsql.uniform(0.5, 1.5));  -- > 0 always
SELECT add_provenance('rc_sensors');
CREATE TABLE rc_result AS
  SELECT id, probability_evaluate(provenance(), 'monte-carlo', '1000') AS p
    FROM rc_sensors WHERE reading > 0;
SELECT remove_provenance('rc_result');
SELECT id, p = 1.0 AS exact_one FROM rc_result ORDER BY id;
DROP TABLE rc_result;
DROP TABLE rc_sensors;

-- ---------------------------------------------------------------
-- Joint-conjunction pass: AND of cmps over a shared continuous RV.
-- The per-cmp pass above leaves these untouched (each cmp is
-- individually inconclusive on its own interval).  The joint pass
-- intersects per-RV intervals over each gate_times's AND-conjunct
-- cmps and resolves the gate_times to gate_zero when any RV's
-- intersection is empty.

-- (J1) Headline: x > 100 AND x < -100 over the SAME N(0, 1).  Each
-- cmp is individually undecidable (normal has unbounded support).
-- Their conjunction constrains x to [100, -100] = empty -> 0 exact.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT provsql.probability_evaluate(
         provsql.provenance_times(
           provsql.rv_cmp_gt(x,  100::random_variable),
           provsql.rv_cmp_lt(x, (-100)::random_variable)),
         'independent') = 0.0
       AS joint_infeasible_normal
FROM r;

-- (J2) Sanity: SATISFIABLE conjunction is NOT pruned.  -1 < x < 1
-- intersects to [-1, 1] which is non-empty, so the joint pass leaves
-- the gate_times intact and the AnalyticEvaluator-multiplied
-- per-cmp result is strictly positive.  Asserting > 0 only checks
-- the joint pass did not falsely prune; the actual numeric value
-- (which uses an independence approximation here) is not the
-- subject of this test.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT provsql.probability_evaluate(
         provsql.provenance_times(
           provsql.rv_cmp_gt(x, (-1)::random_variable),
           provsql.rv_cmp_lt(x,  1::random_variable)),
         'independent') > 0.0
       AS joint_feasible_not_pruned
FROM r;

-- (J3) Uniform: u > 0.8 AND u < 0.2 over u ~ U(0,1).  Each cmp is
-- individually feasible on the [0, 1] support (u > 0.8 covers 20%,
-- u < 0.2 covers 20%); the per-cmp pass therefore cannot resolve.
-- The joint pass intersects [0.8, 1] with [0, 0.2] (each already
-- intersected with the [0, 1] support) -> empty -> 0 exact.  Without
-- the joint pass, 'independent' would return 0.2 * 0.2 = 0.04.
WITH r AS (SELECT provsql.uniform(0, 1) AS u)
SELECT provsql.probability_evaluate(
         provsql.provenance_times(
           provsql.rv_cmp_gt(u, 0.8::random_variable),
           provsql.rv_cmp_lt(u, 0.2::random_variable)),
         'independent') = 0.0
       AS joint_infeasible_uniform
FROM r;

-- (J4) End-to-end through the planner hook: WHERE clause with two
-- conjuncts on the same RV column.  The planner-hook lifts both
-- comparators into provenance_times and the joint pass collapses
-- the gate_times to gate_zero, so probability_evaluate returns 0.0
-- exactly for the surviving row.
CREATE TABLE rc_joint_sensors(id text, reading provsql.random_variable);
INSERT INTO rc_joint_sensors VALUES ('s1', provsql.normal(0, 1));
SELECT add_provenance('rc_joint_sensors');
CREATE TABLE rc_joint_result AS
  SELECT id, probability_evaluate(provenance(), 'independent') AS p
    FROM rc_joint_sensors WHERE reading > 100 AND reading < -100;
SELECT remove_provenance('rc_joint_result');
SELECT id, p = 0.0 AS exact_zero FROM rc_joint_result ORDER BY id;
DROP TABLE rc_joint_result;
DROP TABLE rc_joint_sensors;

SELECT 'ok'::text AS continuous_rangecheck_done;
