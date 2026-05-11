\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- AnalyticEvaluator: closed-form CDFs decide gate_cmps that reduce
-- to (X cmp c) for a bare gate_rv leaf X and bare gate_value c, or
-- (X cmp Y) for two distinct independent normal leaves.  When it
-- decides, the cmp is replaced by a Bernoulli gate_input carrying
-- the analytical probability, so probability_evaluate returns the
-- exact value (independent / treedec methods) or the analytical
-- value plus MC noise (monte-carlo method).
--
-- The 'independent' method is exact when every input gate appears at
-- most once, which is the case after a single-cmp resolution: only
-- the resolved Bernoulli leaf remains.  Use it for the exact
-- assertions below.

-- (1) P(N(0, 1) > 0) = 1 - Phi(0) = 0.5 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.normal(0, 1), 0::random_variable),
         'independent') = 0.5
       AS normal_gt_zero_exact;

-- (2) P(N(0, 1) <= 0) = 0.5 exactly (symmetric).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(provsql.normal(0, 1), 0::random_variable),
         'independent') = 0.5
       AS normal_le_zero_exact;

-- (3) P(U(0, 1) <= 0.3) = 0.3 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(provsql.uniform(0, 1), 0.3::random_variable),
         'independent') = 0.3
       AS uniform_le_03_exact;

-- (4) P(U(0, 10) > 7.5) = 0.25 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.uniform(0, 10), 7.5::random_variable),
         'independent') = 0.25
       AS uniform_gt_75_exact;

-- (5) P(Exp(1) > 1) = 1/e ~= 0.36787944.  Use a tight tolerance
-- (1e-12) to verify Boost.Math's expm1 gives full double precision.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.exponential(1), 1::random_variable),
             'independent') - exp(-1)) < 1e-12
       AS exponential_gt_one_exact;

-- (5b) Erlang closed-form CDF: for Erlang(2, 1), F(x) = 1 - e^{-x}(1+x);
-- P(X > 1) = 2/e, exact via the finite-sum CDF.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.erlang(2, 1), 1::random_variable),
             'independent') - 2.0 * exp(-1.0)) < 1e-12
       AS erlang_gt_exact;

-- (6) P(N(1, 1) > N(0, 1)) for two independent normals.
-- X - Y ~ N(1, sqrt(2)); P(X > Y) = Phi(1/sqrt(2)).
-- Phi(1/sqrt(2)) ~= 0.7602499389...
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(1, 1), provsql.normal(0, 1)),
             'independent') - 0.7602499389065234) < 1e-12
       AS normal_diff_exact;

-- (7) Continuous EQ: P(X = c) = 0 exactly for any continuous X.
-- Resolved by RangeCheck's continuous-EQ/NE shortcut (universal:
-- gate_zero / gate_one are valid in every semiring, so this lives
-- in the load-time pass rather than in AnalyticEvaluator).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.normal(0, 1), 0::random_variable),
         'independent') = 0.0
       AS continuous_eq_zero;

-- (8) Continuous NE: P(X != c) = 1 exactly (complement of EQ).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_ne(provsql.normal(0, 1), 0::random_variable),
         'independent') = 1.0
       AS continuous_ne_one;

-- (8b) Identity shortcut: when both sides of the cmp share the SAME
-- gate UUID (the per-iteration sampler memoisation gives them the
-- same draw), the comparator collapses universally:
--   EQ / LE / GE -> TRUE,  NE / LT / GT -> FALSE.
-- Resolved by RangeCheck's identity shortcut, also universal.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT
  provsql.probability_evaluate(provsql.rv_cmp_eq(x, x), 'independent') = 1.0 AS eq_x_x,
  provsql.probability_evaluate(provsql.rv_cmp_ne(x, x), 'independent') = 0.0 AS ne_x_x,
  provsql.probability_evaluate(provsql.rv_cmp_le(x, x), 'independent') = 1.0 AS le_x_x,
  provsql.probability_evaluate(provsql.rv_cmp_lt(x, x), 'independent') = 0.0 AS lt_x_x,
  provsql.probability_evaluate(provsql.rv_cmp_ge(x, x), 'independent') = 1.0 AS ge_x_x,
  provsql.probability_evaluate(provsql.rv_cmp_gt(x, x), 'independent') = 0.0 AS gt_x_x
FROM r;

-- (9) Reversed-operand shape: c > X is X < c.
-- P(0 > N(0, 1)) = P(N(0, 1) < 0) = 0.5.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(0::random_variable, provsql.normal(0, 1)),
         'independent') = 0.5
       AS const_cmp_rv_exact;

-- (10a) Composition that AnalyticEvaluator does NOT handle: X cmp c
-- where X is a gate_arith (not a bare gate_rv).  Negative-space
-- check: if AnalyticEvaluator had silently resolved it (wrongly),
-- the cmp would have been replaced by a Bernoulli gate_input and
-- the 'independent' method would compute a probability without
-- error.  Because AnalyticEvaluator correctly skips this shape,
-- the cmp remains in the circuit; the 'independent' method routes
-- through the BoolExpr semiring whose cmp() throws -- and the
-- error proves the cmp was not resolved.
\set VERBOSITY terse
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.normal(0, 1) + provsql.normal(0, 1),
                            0::random_variable),
         'independent');
\set VERBOSITY default

-- (10b) Same shape, this time via 'monte-carlo': the RV-aware MC
-- sampler handles gate_arith / gate_rv natively (it is the
-- fall-through path for what AnalyticEvaluator does not decide),
-- and gives the right answer within sampling noise.
-- N(0,1) + N(0,1) ~ N(0, sqrt(2)); P(sum > 0) = 0.5 by symmetry.
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1) + provsql.normal(0, 1),
                                0::random_variable),
             'monte-carlo', '100000') - 0.5) < 0.01
       AS arith_falls_through_to_mc;
RESET provsql.monte_carlo_seed;

-- (11) End-to-end through the planner hook: WHERE rv > c gets
-- lifted, then RangeCheck doesn't decide (normal has unbounded
-- support), then AnalyticEvaluator resolves to gate_input(p).
-- Each row's provsql is the resolved Bernoulli; probability_evaluate
-- returns the exact CDF answer.
CREATE TABLE ae_sensors(id text, reading provsql.random_variable);
INSERT INTO ae_sensors VALUES
  ('a', provsql.normal(2.5, 0.5)),
  ('b', provsql.uniform(1, 3));
SELECT add_provenance('ae_sensors');
CREATE TABLE ae_result AS
  SELECT id, probability_evaluate(provenance(), 'independent') AS p
    FROM ae_sensors WHERE reading > 2;
SELECT remove_provenance('ae_result');
SELECT id,
       abs(p - CASE id WHEN 'a' THEN 0.8413447460685429
                       WHEN 'b' THEN 0.5 END) < 1e-12
       AS within_tolerance
  FROM ae_result ORDER BY id;
DROP TABLE ae_result;
DROP TABLE ae_sensors;

SELECT 'ok'::text AS continuous_analytic_done;
