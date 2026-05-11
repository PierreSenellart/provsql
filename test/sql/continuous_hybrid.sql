\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- HybridEvaluator simplifier: peephole rewrites on gate_arith
-- subtrees that preserve the gate's scalar value in every world.
-- The probability path runs RangeCheck -> simplifier -> RangeCheck
-- (re-pass picks up newly-folded constants in the joint-conjunction
-- check) -> AnalyticEvaluator, so the assertions below exercise the
-- closure rules end-to-end through probability_evaluate('independent')
-- with the analytical exact tolerance (1e-12).

-- ---------------------------------------------------------------
-- Normal-family closure: linear combinations of independent normals
-- fold to a single normal whose mean and variance are the closed-form
-- combinations.  Independence is tested via the base-RV footprint;
-- the underlying gate_rv UUIDs being distinct (each provsql.normal
-- call mints a fresh v4 UUID) is sufficient.
-- ---------------------------------------------------------------

-- (1) N(0, 1) + N(0, 1) folds to N(0, sqrt(2)); P(sum > 1) =
--     1 - Phi(1/sqrt(2)) ~= 0.2398.  TODO's core 7(a) case.
--     Threshold chosen off the fold's mean so the test discriminates
--     a wrong-variance closure (e.g. an erroneous fold to N(0, 1)
--     would give 0.1587, well outside the 1e-12 tolerance).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1) + provsql.normal(0, 1),
                                1::random_variable),
             'independent') - 0.23975006109347664) < 1e-12
       AS sum_two_indep_normals_exact;

-- (2) 3 * N(0, 1) + N(0, 4) folds to N(0, sqrt(9 + 16)) = N(0, 5);
--     P(... > 5) = 1 - Phi(1) ~= 0.1587.  Parameters chosen so the
--     fold's sigma is integer-clean and the threshold (one sigma
--     above the mean) discriminates a wrong scalar coefficient
--     (e.g. if try_normal_closure misread the TIMES wires and used
--     a = 1 instead of a = 3, the fold would be N(0, sqrt(17))
--     giving P ~= 0.113, outside tolerance).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(3::random_variable * provsql.normal(0, 1)
                              + provsql.normal(0, 4),
                               5::random_variable),
             'independent') - 0.15865525393145707) < 1e-12
       AS scaled_normal_sum_exact;

-- (3) NEG of a normal inside the linear combination: -N(2, 1) + N(2, 1)
--     folds to N(0, sqrt(2)).  Threshold > 1 again, same truth as (1)
--     by symmetry: a wrong sign on the NEG decomposition (a = +1
--     instead of -1) would fold to N(4, sqrt(2)), giving
--     P(N(4, sqrt(2)) > 1) ~= 0.983, far outside tolerance.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(-provsql.normal(2, 1) + provsql.normal(2, 1),
                                1::random_variable),
             'independent') - 0.23975006109347664) < 1e-12
       AS neg_normal_in_closure_exact;

-- (4) Dependent normals: PLUS over the SAME N(0, 1) twice MUST NOT
--     trigger the closure (the independence test in try_normal_closure
--     rejects shared UUIDs).  The threshold is chosen to discriminate
--     between the correct (dependent) answer and the would-be-wrong
--     answer if the closure had fired:
--     - dependent truth:    P(2x > 1) = P(x > 0.5) = 1 - Phi(0.5)
--                                      ≈ 0.3085
--     - wrongful fold:      P(N(0, sqrt(2)) > 1) = 1 - Phi(1/sqrt(2))
--                                      ≈ 0.2398
--     The 0.01 MC tolerance around 0.3085 is well outside 0.2398, so
--     a wrongful fold would fail the assertion.  This also indirectly
--     exercises the sampler's per-iteration scalar memoisation (x must
--     produce the same draw on both PLUS wires).
SET provsql.monte_carlo_seed = 42;
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(x + x, 1::random_variable),
             'monte-carlo', '100000') - 0.3085375387259869) < 0.01
       AS dependent_normals_not_folded
FROM r;
RESET provsql.monte_carlo_seed;

-- ---------------------------------------------------------------
-- Erlang-family closure: PLUS over k i.i.d. Exp(λ) leaves with the
-- same rate λ and distinct UUIDs folds to a single Erlang(k, λ).
-- ---------------------------------------------------------------

-- (5) Three i.i.d. Exp(1) ~ Erlang(3, 1); P(sum > 3) = e^{-3}(1+3+4.5)
--     = 8.5 / e^3.  Exact via simplifier + analytic CDF.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.exponential(1)
                              + provsql.exponential(1)
                              + provsql.exponential(1),
                               3::random_variable),
             'independent') - 8.5 * exp(-3.0)) < 1e-12
       AS erlang_fold_exact;

-- (6) Mixed rates: must NOT fold (hypoexponential is outside the
--     closure's scope - the i.i.d.-rate guard inside try_erlang_closure
--     rejects this case).  Falls through to MC; tolerance check
--     verifies the path executed without error.
SET provsql.monte_carlo_seed = 42;
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.exponential(1) + provsql.exponential(2),
                            1.5::random_variable),
         'monte-carlo', '100000') BETWEEN 0.3 AND 0.7
       AS mixed_rates_not_folded;
RESET provsql.monte_carlo_seed;

-- ---------------------------------------------------------------
-- Constant folding + identity drops.
-- ---------------------------------------------------------------

-- (7) Constant folding of arith(NEG, value:1) -> value:-1.  Before
--     the simplifier, AnalyticEvaluator's "X cmp constant" shortcut
--     would skip a cmp whose constant side was wrapped in NEG,
--     forcing tests to write (-1)::random_variable instead of
--     -1::random_variable.  After the fold the branch fires on the
--     natural form.  P(N(0, 1) > -1) = 1 - Phi(-1) = Phi(1)
--     ~= 0.8413; threshold deliberately within (0, 1) so the test
--     discriminates a wrong sign on the NEG fold (which would give
--     P(N(0, 1) > 1) ~= 0.1587, far outside tolerance).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1),
                                -(1::random_variable)),
             'independent') - 0.8413447460685429) < 1e-12
       AS constant_fold_neg_value;

-- (8) Identity-element drop unlocks the normal closure: the inner
--     `N(0, 1) + 0::random_variable` identity-drops to a singleton
--     PLUS wrapper around N(0, 1); decompose_normal_term recurses
--     through that wrapper, so the outer `+ N(0, 1)` PLUS folds to
--     N(0, sqrt(2)) and the cmp resolves analytically.  Same
--     threshold/truth as (1) so a wrong-variance closure (or a
--     missed identity drop that left the value:0 wire and broke
--     decomposition) would fail the tolerance check.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1)
                              + 0::random_variable
                              + provsql.normal(0, 1),
                                1::random_variable),
             'independent') - 0.23975006109347664) < 1e-12
       AS identity_drop_unlocks_normal_closure;

-- (9) Zero absorber on TIMES: arith(TIMES, value:0, X) folds to
--     gate_value:0 regardless of X, via the same identity-drop pass.
--     The resulting cmp `0 > 0.5` is then RangeCheck-decided to 0.0
--     exactly.  Threshold 0.5 deliberately falls between 0 and 1 so
--     a wrong fold to gate_value:1 would flip the cmp to 1 > 0.5
--     = 1.0; the `= 0.0` assertion catches that.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(0::random_variable * provsql.normal(0, 1),
                            0.5::random_variable),
         'independent') = 0.0
       AS zero_absorber_times;

-- ---------------------------------------------------------------
-- Sanity: with provsql.hybrid_evaluation = off the simplifier does
-- not run.  The same queries fall through to the original paths:
-- - The normal-sum cmp reaches the BoolExpr semiring without a
--   resolved Bernoulli, which throws (`This semiring does not
--   support value gates.`).
-- - The same query via 'monte-carlo' still works via the RV-aware
--   MC sampler over gate_arith / gate_rv, within sampling tolerance.
-- ---------------------------------------------------------------

SET provsql.hybrid_evaluation = off;

\set VERBOSITY terse
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(provsql.normal(0, 1) + provsql.normal(0, 1),
                            0::random_variable),
         'independent');
\set VERBOSITY default

SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.normal(0, 1) + provsql.normal(0, 1),
                                0::random_variable),
             'monte-carlo', '100000') - 0.5) < 0.01
       AS hybrid_off_falls_through_to_mc;
RESET provsql.monte_carlo_seed;

RESET provsql.hybrid_evaluation;

SELECT 'ok'::text AS continuous_hybrid_done;
