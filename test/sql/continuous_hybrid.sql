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

-- (4) Shared-UUID PLUS: x + x aggregates via try_plus_aggregate to
--     `2 * x`, which then folds via try_times_scalar_rv to N(0, 2).
--     The cmp resolves analytically off that single bare gate_rv:
--     P(2x > 1) = P(x > 0.5) = 1 - Phi(0.5) ≈ 0.3085.  A wrongful
--     independence-assumed fold (independent-normal closure firing
--     before aggregation) would give N(0, sqrt(2)) and P ≈ 0.2398,
--     well outside the 1e-12 tolerance.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(x + x, 1::random_variable),
             'independent') - 0.3085375387259869) < 1e-12
       AS shared_uuid_plus_collapses_to_2X
FROM (SELECT provsql.normal(0, 1) AS x) r;

-- (4b) Mixed coefficients on the same RV: `2*x + x` aggregates to
--     `3*x` (coefficient sum), then folds to N(0, 3) via the scalar
--     closure.  P(3x > 3) = P(x > 1) = 1 - Phi(1) ≈ 0.1587.  A miss of
--     the coefficient sum (e.g. dropping the inner 2*x's coefficient
--     and treating it as just `x`) would fold to N(0, 2) and give
--     P ≈ 0.3085, well outside tolerance.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(2::random_variable * x + x,
                                3::random_variable),
             'independent') - 0.15865525393145707) < 1e-12
       AS shared_uuid_plus_mixed_coeffs
FROM (SELECT provsql.normal(0, 1) AS x) r;

-- (4c) Coefficient cancellation: `x + (-x)` aggregates to coefficient
--     0 on x, collapsing the PLUS to gate_value:0.  The cmp `0 > 0`
--     resolves to P = 0 via RangeCheck on the value gate.  A miss of
--     the NEG sign in the linear-term decomposition would aggregate
--     to 2*x or keep the PLUS unfolded, giving 0.5 / 0.5 respectively.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(x + (-x), 0::random_variable),
         'independent') = 0.0
       AS shared_uuid_plus_cancels_to_zero
FROM (SELECT provsql.normal(0, 1) AS x) r;

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
-- Categorical N-wire mixture lift: push constant scaling / offset
-- inside the [key, mul_1, ..., mul_n] form by minting new mulinputs
-- with transformed value extras.  Shares the key gate with the
-- original so the lifted mixture stays perfectly correlated with the
-- input (FootprintCache flags the shared key as a dependency atom).
-- ---------------------------------------------------------------

-- (10a) TIMES lift on categorical: 3 * cat({0.5, 0.5}, {2, 4}) folds
--      to cat({0.5, 0.5}, {6, 12}).  P(>7) = 0.5 (mass on outcome 12)
--      decided exactly via AnalyticEvaluator's categoricalDecide.  A
--      wrong fold (e.g. missing the scaling, or using factor=1) would
--      give P(>7) = 0 on the original {2, 4} outcomes, far outside
--      the bit-equal 0.5 assertion.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(
           3::random_variable * provsql.categorical(ARRAY[0.5, 0.5]::float[],
                                                     ARRAY[2.0, 4.0]::float[]),
           7::random_variable),
         'independent') = 0.5
       AS cat_lift_scaled_exact;

-- (10b) PLUS lift on categorical: 10 + cat({0.5, 0.5}, {2, 4}) folds
--      to cat({0.5, 0.5}, {12, 14}).  P(>13) = 0.5 (mass on outcome
--      14) decided exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(
           10::random_variable + provsql.categorical(ARRAY[0.5, 0.5]::float[],
                                                      ARRAY[2.0, 4.0]::float[]),
           13::random_variable),
         'independent') = 0.5
       AS cat_lift_shifted_exact;

-- (10c) Negative scale on categorical (allowed; flips support but the
--      categorical stays well-formed since outcomes are explicit
--      points).  -2 * cat({0.5, 0.5}, {2, 4}) folds to
--      cat({0.5, 0.5}, {-4, -8}).  P(>-5) = 0.5 (mass on outcome -4).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(
           (-2)::random_variable * provsql.categorical(ARRAY[0.5, 0.5]::float[],
                                                       ARRAY[2.0, 4.0]::float[]),
           (-5)::random_variable),
         'independent') = 0.5
       AS cat_lift_neg_scale_exact;

-- (10d) Shared-mixture PLUS aggregation: `x+x` where x is a
--      categorical folds via try_plus_aggregate (now treating
--      gate_mixture as a scalar-RV leaf) to `2*x`, which the
--      categorical mixture lift then folds to a new categorical with
--      doubled outcome values.  P(2*x > 3) for x in {1, 2} = mass of
--      outcome 4 = 0.5.  Without the gate_mixture leaf case the
--      aggregator would bail and the PLUS would stay unfolded, falling
--      through to MC.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_gt(x + x, 3::random_variable),
         'independent') = 0.5
       AS cat_shared_mixture_plus_aggregate
FROM (SELECT provsql.categorical(ARRAY[0.5, 0.5]::float[],
                                  ARRAY[1.0, 2.0]::float[]) AS x) r;

-- ---------------------------------------------------------------
-- Island decomposer: per-cmp MC marginalisation of unresolved
-- continuous-island cmps into Bernoulli gate_input leaves.  Picks
-- up cmps whose shape none of RangeCheck / simplifier /
-- AnalyticEvaluator can resolve - e.g. heterogeneous distributions
-- under arith (the normal closure rejects non-normal wires).
-- Single-cmp islands only; cmps sharing a base RV with another
-- unresolved cmp are detected via footprint overlap and skipped.
-- ---------------------------------------------------------------

-- (12) Disjoint islands.  Two independent N(0,1) + U(-1,1) mixes,
--      neither folded by the simplifier (U isn't normal, so the
--      normal closure bails).  The decomposer MC-marginalises each
--      cmp into its own Bernoulli; 'independent' then computes
--      inclusion-exclusion over them.  By the symmetry of N(0,1)
--      and U(-1,1) around zero, P(N + U > 0) = 0.5 exactly, so the
--      analytical truth of the disjunction is 0.5 + 0.5 - 0.25
--      = 0.75.  Without the decomposer, 'independent' would error
--      on the unresolved gate_arith leaves.
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 20000;
SELECT abs(provsql.probability_evaluate(
             provsql.provenance_plus(ARRAY[
               provsql.rv_cmp_gt(provsql.normal(0, 1)
                                + provsql.uniform(-1, 1),
                                  0::random_variable),
               provsql.rv_cmp_gt(provsql.normal(0, 1)
                                + provsql.uniform(-1, 1),
                                  0::random_variable)
             ]),
             'independent') - 0.75) < 0.02
       AS disjoint_islands_inclusion_exclusion;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

-- (13) Shared island, joint-table inline via 'monte-carlo'.  Two
--      cmps share the SAME (N + U) arith gate (via a CTE binding so
--      r.expr is the same gate_t in both cmp_gt calls).  The
--      dependent truth is P(x > 0 OR x > 1) = P(x > 0) = 0.5 by the
--      subset relation `x > 1 implies x > 0`.  The decomposer
--      detects the shared footprint, samples the joint distribution
--      over (cmp_A, cmp_B), and inlines a 4-way mulinput block;
--      cmp_A and cmp_B are rewritten as gate_plus over the mulinputs
--      with their bit set.  After rewriteMultivaluedGates (called
--      inside the legacy 'monte-carlo' path) the Bayesian tree
--      preserves the joint dependence, so the BC monteCarlo over
--      the OR gives ~0.5.  If the decomposer had wrongly treated
--      the two cmps as independent, each marginal Bernoulli (0.5
--      and ~0.2) OR'd independently would give ~0.6 - well outside
--      tolerance.
SET provsql.monte_carlo_seed = 42;
WITH r AS (SELECT provsql.normal(0, 1) + provsql.uniform(-1, 1) AS expr)
SELECT abs(provsql.probability_evaluate(
             provsql.provenance_plus(ARRAY[
               provsql.rv_cmp_gt(r.expr, 0::random_variable),
               provsql.rv_cmp_gt(r.expr, 1::random_variable)
             ]),
             'monte-carlo', '100000') - 0.5) < 0.01
       AS shared_island_joint_table_mc
FROM r;
RESET provsql.monte_carlo_seed;

-- (14) Same shared-island shape via 'tree-decomposition'.  The
--      tree-decomposition method calls rewriteMultivaluedGates
--      on the joint-table mulinputs and then runs the
--      d-DNNF-based evaluator on the resulting Boolean circuit,
--      which handles repeated MULIN references across cmps
--      cleanly (the Bayesian-tree rewrite re-shares the per-block
--      Bernoulli decisions across cmp_A and cmp_B by construction).
--      The result should match the dependent truth (0.5) within
--      the joint-table MC noise.
SET provsql.monte_carlo_seed = 42;
WITH r AS (SELECT provsql.normal(0, 1) + provsql.uniform(-1, 1) AS expr)
SELECT abs(provsql.probability_evaluate(
             provsql.provenance_plus(ARRAY[
               provsql.rv_cmp_gt(r.expr, 0::random_variable),
               provsql.rv_cmp_gt(r.expr, 1::random_variable)
             ]),
             'tree-decomposition') - 0.5) < 0.02
       AS shared_island_joint_table_treedec
FROM r;
RESET provsql.monte_carlo_seed;

-- (15) Multi-cmp shared island where the cmps do NOT share a
--      common scalar lhs.  Two cmps `(X + Y_A) > 0` and
--      `(X + Y_B) > 0` share base RV X (their footprints overlap
--      on X) but use different arith composites (Y_A and Y_B are
--      distinct fresh uniforms, so the lhs gate_arith UUIDs
--      differ).  The monotone-shared-scalar fast path's
--      detect_shared_scalar checks for an identical lhs gate_t
--      and bails on this case; the generic 2^k MC joint table is
--      the only correct handler.  Test pins the dependent truth
--      ~ 0.621 against the wrong-independent ~ 0.75 to detect a
--      regression where detection wrongly accepts disparate
--      scalars (e.g. a future canonicalisation that conflates
--      different arith subtrees with the same base-RV footprint).
--
--      With X ~ N(0, 1) and Y_A, Y_B ~ U(-1, 1) all independent,
--      P(A) = P(B) = 0.5 by symmetry; conditional on X = x the two
--      cmps are independent, so P(A AND B) integrates
--      `clip((1 + x) / 2, 0, 1)^2` against the standard normal
--      density, yielding ~0.379.  P(A OR B) ~= 0.621.  The
--      independent-Bernoulli answer would be 0.5 + 0.5 - 0.25
--      = 0.75 - well outside the 0.05 tolerance.
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 20000;
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT abs(provsql.probability_evaluate(
             provsql.provenance_plus(ARRAY[
               provsql.rv_cmp_gt(r.x + provsql.uniform(-1, 1),
                                 0::random_variable),
               provsql.rv_cmp_gt(r.x + provsql.uniform(-1, 1),
                                 0::random_variable)
             ]),
             'tree-decomposition') - 0.621) < 0.05
       AS shared_island_disparate_scalars
FROM r;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

-- (16) Fast-path analytical CDF on a shared bare-RV scalar.  Two
--      cmps `X > 0` and `X > 1` over the SAME N(0, 1) (CTE-bound,
--      so r.x is the same gate_t in both cmps).  The decomposer
--      runs before AnalyticEvaluator so the shared footprint
--      groups the cmps; detect_shared_scalar matches (same lhs
--      gate_t, gate_value rhs) and the fast path computes the 3
--      interval probabilities via cdfAt on the normal CDF -
--      analytically, no MC anywhere:
--        P((-inf, 0])  = Phi(0)      = 0.5
--        P((0, 1])     = Phi(1) - 0.5 ~ 0.34134
--        P((1, +inf))  = 1 - Phi(1)   ~ 0.15866
--      P(X > 0 OR X > 1) = P(X > 0) = 0.5 exactly by the subset
--      relation `x > 1 implies x > 0`, so 'tree-decomposition' over
--      the joint mulinput block recovers 0.5 to float8 precision.
--      Test asserts bit-equal 0.5 (no tolerance) - this assertion
--      is only achievable via the analytical branch; if the fast
--      path bailed to MC binning or the analytical CDF were
--      miscomputed, the result would land off-bit.  The wrong-
--      independent answer (silent AnalyticEvaluator pre-emption
--      before the reorder) would give 0.5 + 0.1587 - 0.5*0.1587
--      ~ 0.579, far from 0.5.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT provsql.probability_evaluate(
         provsql.provenance_plus(ARRAY[
           provsql.rv_cmp_gt(r.x, 0::random_variable),
           provsql.rv_cmp_gt(r.x, 1::random_variable)
         ]),
         'tree-decomposition') = 0.5
       AS shared_bare_rv_fast_path_exact
FROM r;

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

-- ---------------------------------------------------------------
-- Shortest round-trip in double_to_text: scalar parameters written
-- back into a folded gate_rv's extra should print as the shortest
-- decimal that round-trips through std::stod, not the verbose
-- precision-17 form.  E.g. `2 * Exp(0.4)` folds to Exp(0.2); the
-- extra should be "exponential:0.2", not "exponential:0.20000000000000001".
-- This pins the behaviour of the std::to_chars-based formatter.
-- The simplifier runs against the in-memory GenericCircuit returned
-- by getGenericCircuit, so we read the folded extra via
-- simplified_circuit_subgraph (which dispatches through the loader).
-- ---------------------------------------------------------------
WITH q AS (SELECT 2 * provsql.exponential(0.4) AS r),
     j AS (
       SELECT provsql.simplified_circuit_subgraph(
                (r)::uuid, 1)::text AS s
         FROM q
     )
SELECT s LIKE '%"extra": "exponential:0.2"%'
       AS double_to_text_shortest_roundtrip
FROM j;

-- ---------------------------------------------------------------
-- Affine closures: MINUS-to-PLUS canonicalisation, NEG-of-RV on
-- Normal / Uniform, Uniform-family closure with a constant offset.
--
-- The simplifier rewrites @c MINUS(A, B) as @c PLUS(A, NEG(B)) up
-- front, then the existing normal-family / Erlang-family closures
-- and the new uniform-family closure handle the resulting PLUS.
-- @c try_neg_rv (pass 2) folds a standalone @c NEG over a bare
-- Normal / Uniform leaf; it bails on Exp / Erlang (negative support
-- is not those families).
--
-- We pin via @c simplified_circuit_subgraph at depth 1: when a fold
-- succeeds the root collapses to a single bare @c gate_rv, so the
-- depth-1 view's only row is the root itself with the expected
-- gate_type and extra.  Negative cases assert the root stays
-- @c gate_arith.
-- ---------------------------------------------------------------

-- (1) -N(2, 0.5) folds to N(-2, 0.5).
WITH q AS (SELECT - provsql.normal(2, 0.5) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'normal:-2,0.5'
       AS neg_normal_folds
FROM j;

-- (2) -U(1, 3) folds to U(-3, -1).
WITH q AS (SELECT - provsql.uniform(1, 3) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'uniform:-3,-1'
       AS neg_uniform_folds
FROM j;

-- (3) N(2, 0.5) - 1 folds to N(1, 0.5).
WITH q AS (SELECT provsql.normal(2, 0.5) - 1::random_variable AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'normal:1,0.5'
       AS normal_minus_constant_folds
FROM j;

-- (4) 1 - N(2, 0.5) folds to N(-1, 0.5).
WITH q AS (SELECT 1::random_variable - provsql.normal(2, 0.5) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'normal:-1,0.5'
       AS constant_minus_normal_folds
FROM j;

-- (5) U(1, 3) - 0.5 folds to U(0.5, 2.5).
WITH q AS (SELECT provsql.uniform(1, 3) - 0.5::random_variable AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'uniform:0.5,2.5'
       AS uniform_minus_constant_folds
FROM j;

-- (6) 0.5 - U(1, 3) folds to U(-2.5, -0.5).  Exercises the chain
--     MINUS -> PLUS(value, NEG(U)) -> uniform closure with a == -1.
WITH q AS (SELECT 0.5::random_variable - provsql.uniform(1, 3) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'uniform:-2.5,-0.5'
       AS constant_minus_uniform_folds
FROM j;

-- (7) U(1, 3) + 2 folds to U(3, 5).
WITH q AS (SELECT provsql.uniform(1, 3) + 2::random_variable AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'rv'
       AND (s -> 0 ->> 'extra') = 'uniform:3,5'
       AS uniform_plus_constant_folds
FROM j;

-- Negative (8): -Exp(0.4) keeps a NEG gate (negative support is no
-- longer exponential).
WITH q AS (SELECT - provsql.exponential(0.4) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'arith'
       AND (s -> 0 ->> 'info1') = '4'  -- PROVSQL_ARITH_NEG
       AS neg_exp_stays
FROM j;

-- Negative (9): Exp + c keeps a PLUS gate (shifted exponential is no
-- longer exponential).
WITH q AS (SELECT provsql.exponential(0.4) + 1::random_variable AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'arith'
       AND (s -> 0 ->> 'info1') = '0'  -- PROVSQL_ARITH_PLUS
       AS exp_plus_constant_stays
FROM j;

-- Negative (10): U + U with distinct uniforms keeps a PLUS gate
-- (sum of two independent uniforms is triangular, not uniform).
WITH q AS (SELECT provsql.uniform(0, 1) + provsql.uniform(0, 1) AS r),
     j AS (SELECT provsql.simplified_circuit_subgraph(
                    (r)::uuid, 1)::jsonb AS s
              FROM q)
SELECT (s -> 0 ->> 'gate_type') = 'arith'
       AND (s -> 0 ->> 'info1') = '0'  -- PROVSQL_ARITH_PLUS
       AS uniform_plus_uniform_stays
FROM j;

-- End-to-end: the new -N fold unlocks an AnalyticEvaluator path.
-- P(-N(2.5, 0.5) < -2) = P(N(2.5, 0.5) > 2) = 1 - Phi((2-2.5)/0.5)
-- = 1 - Phi(-1) = Phi(1) ~= 0.8413.  Before the new -N fold the
-- comparator would have flowed to the MC marginalisation; after the
-- simplifier folds -N(2.5, 0.5) to N(-2.5, 0.5), AnalyticEvaluator
-- resolves the cmp via the closed-form normal CDF.  The 1e-12
-- tolerance is the analytical-exact tolerance, only achievable when
-- the analytical path actually fires.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(-provsql.normal(2.5, 0.5),
                                (-2)::random_variable),
             'independent') - 0.8413447460685429) < 1e-12
       AS neg_normal_unlocks_analytic;

SELECT 'ok'::text AS continuous_hybrid_done;
