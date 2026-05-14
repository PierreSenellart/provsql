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

-- (3b) Erlang inherits the [0, +inf) support; Erlang(3, 1) < 0 is
-- always false.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_lt(provsql.erlang(3, 1), 0::random_variable),
         'monte-carlo', '1000000') = 0.0
       AS erlang_strictly_negative_decided;

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
-- the gate_times intact.  Asserting > 0 only checks the joint pass
-- did not falsely prune.  Uses 'tree-decomposition' because the
-- hybrid decomposer's fast path now groups the two shared-X cmps
-- and inlines a joint-table mulinput block, which 'independent'
-- correctly rejects as a dependent circuit ("Not an independent
-- circuit") when the cmps combine via AND.  The
-- tree-decomposition path handles the dependent circuit and
-- recovers the exact P(-1 < X < 1) = 2*Phi(1) - 1 ~= 0.6827.
WITH r AS (SELECT provsql.normal(0, 1) AS x)
SELECT provsql.probability_evaluate(
         provsql.provenance_times(
           provsql.rv_cmp_gt(x, (-1)::random_variable),
           provsql.rv_cmp_lt(x,  1::random_variable)),
         'tree-decomposition') > 0.0
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

-- ---------------------------------------------------------------
-- Broadened continuous EQ / NE shortcut.
--
-- RangeCheck recognises @c P(X = Y) = 0 / @c P(X != Y) = 1 not just
-- for bare @c gate_rv leaves but for every sub-circuit that produces
-- a continuous distribution: @c gate_arith composites whose leaves
-- are all @c gate_rv, and Bernoulli mixtures over two continuous
-- arms.  Categorical mixtures and pure @c gate_value Diracs fall
-- through to the agg / interval / AnalyticEvaluator paths (the
-- former has point masses; the latter is bit-comparable to a
-- literal).
--
-- The exact 0.0 / 1.0 assertions below would not hold if MC ran on
-- top, so they pin the analytical resolution.
-- ---------------------------------------------------------------

-- (E1) Heterogeneous-rate exponential sum: no Erlang closure exists,
-- so the simplifier cannot fold to a bare @c gate_rv.  RangeCheck's
-- broadened EQ shortcut sees a gate_arith composite whose every leaf
-- is gate_rv and collapses the cmp to gate_zero.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.exponential(0.4)
                           + provsql.exponential(0.3),
                           1::random_variable),
         'independent') = 0.0
       AS heterogeneous_exp_sum_eq;

-- (E2) Product of two independent continuous RVs: also no closure,
-- but every leaf below the gate_arith is gate_rv so EQ collapses
-- exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.normal(0, 1) * provsql.normal(0, 1),
                            0::random_variable),
         'independent') = 0.0
       AS product_two_normals_eq;

-- (E3) Bernoulli mixture over two continuous arms is itself a
-- continuous distribution (convex combination of two continuous
-- densities, no point mass).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.mixture(0.3, provsql.normal(0, 1),
                                                  provsql.uniform(-1, 1)),
                            0::random_variable),
         'independent') = 0.0
       AS bernoulli_mixture_continuous_arms_eq;

-- (E4) NE is symmetric: same shapes resolve to 1.0 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_ne(provsql.exponential(0.4)
                           + provsql.exponential(0.3),
                           1::random_variable),
         'independent') = 1.0
       AS heterogeneous_exp_sum_ne;

-- (E5) Mixture-of-arith and arith-of-mixture both qualify: the
-- recursion through gate_arith into gate_mixture's branches sees
-- two continuous arms.  A common shape after the simplifier's
-- mixture-lift fold.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.mixture(0.5, provsql.normal(0, 1)
                                                + provsql.uniform(0, 1),
                                                  provsql.exponential(1)
                                                + provsql.uniform(0, 1)),
                            0::random_variable),
         'independent') = 0.0
       AS mixture_of_arith_continuous_eq;

-- ---- Negative cases (shortcut does NOT fire) -------------------

-- (E6) Categorical is a discrete distribution: it has a Dirac mass
-- at 0 with weight 0.5, so P(cat = 0) = 0.5, not 0.  The shortcut
-- must distinguish the Bernoulli mixture shape (3-wire) from the
-- categorical N-wire shape via @c isCategoricalMixture.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.categorical(ARRAY[0.5, 0.5],
                                                ARRAY[0.0, 1.0]),
                            0::random_variable),
         'independent') = 0.5
       AS categorical_dirac_eq_not_shorted;

-- (E7) Pure Dirac (@c as_random) compared to itself: handled by the
-- identity shortcut (wires[0] == wires[1]); compared to a literal of
-- the same value, the cmp drops out via the constant comparator and
-- the broadened predicate does not fire on the gate_value side.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(provsql.as_random(2),
                            2::random_variable),
         'independent') = 1.0
       AS dirac_eq_constant;

-- ---------------------------------------------------------------
-- Exact EQ / NE via Dirac mass-map sum-product.
--
-- When both sides of an EQ / NE cmp have statically-extractable
-- @c (value -> mass) Dirac maps and are independent (random-leaf
-- footprints disjoint), RangeCheck computes
--   @c P(X = Y) = Σ_{v ∈ M_X ∩ M_Y} M_X[v] · M_Y[v]
-- exactly and resolves the cmp to a Bernoulli with that probability.
-- This generalises the disjoint-Diracs case (sum is 0) to overlapping
-- Diracs with any mass distribution, and covers categoricals,
-- Bernoulli mixtures with as_random branches, and combinations.
--
-- The exact = 0.0 / 1.0 assertions pin analytical resolution (MC
-- would have finite-sample noise); the fractional cases use exact
-- equality on the closed-form sum-product.
-- ---------------------------------------------------------------

-- (F1) Disjoint categoricals fold to 0 exactly.  The shortcut's
-- common boundary case.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(
           provsql.categorical(ARRAY[0.5, 0.5], ARRAY[0.0, 1.0]),
           provsql.categorical(ARRAY[0.5, 0.5], ARRAY[2.0, 3.0])),
         'independent') = 0.0
       AS disjoint_categoricals_eq_zero;

-- (F2) Categoricals with one outcome in common (value 1) and
-- distinct supports otherwise: P(=) = 0.5 * 0.3 = 0.15 exactly.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(
           provsql.categorical(ARRAY[0.5, 0.5], ARRAY[0.0, 1.0]),
           provsql.categorical(ARRAY[0.3, 0.7], ARRAY[1.0, 2.0])),
         'independent') = 0.15
       AS overlapping_categoricals_one_match;

-- (F3) Categoricals with identical outcome sets but different masses:
-- P(=) = 0.4·0.7 + 0.6·0.3 = 0.46.  A floating-point sum, so we
-- compare with a tight 1e-12 tolerance against the exact value -- the
-- analytical path is bit-deterministic, MC would not reach 1e-12.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_eq(
               provsql.categorical(ARRAY[0.4, 0.6], ARRAY[0.0, 1.0]),
               provsql.categorical(ARRAY[0.7, 0.3], ARRAY[0.0, 1.0])),
             'independent') - 0.46) < 1e-12
       AS same_outcomes_different_masses_eq;

-- (F4) Bernoulli mixture of Diracs vs another mixture of Diracs:
-- M_lhs = {0: 0.3, 5: 0.7}, M_rhs = {0: 0.5, 7: 0.5}.
-- Only value 0 overlaps: P(=) = 0.3 · 0.5 = 0.15.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_eq(
           provsql.mixture(0.3, provsql.as_random(0), provsql.as_random(5)),
           provsql.mixture(0.5, provsql.as_random(0), provsql.as_random(7))),
         'independent') = 0.15
       AS mixture_of_diracs_overlap_eq;

-- (F5) NE: symmetric to (F4): 1 - 0.15 = 0.85.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_ne(
           provsql.mixture(0.3, provsql.as_random(0), provsql.as_random(5)),
           provsql.mixture(0.5, provsql.as_random(0), provsql.as_random(7))),
         'independent') = 0.85
       AS mixture_of_diracs_overlap_ne;

-- (F6) Mixture with one continuous and one Dirac arm vs categorical.
-- LHS = mixture(0.3, N(0,1), as_random(5)) has Dirac mass at 5 with
-- weight 0.7 (the continuous arm contributes no Dirac mass; vs the
-- categorical's discrete points, that arm gives 0 by measure zero).
-- RHS = categorical([0.5, 0.5], [5, 6]).  Overlap at 5: 0.7 · 0.5 = 0.35.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_eq(
               provsql.mixture(0.3, provsql.normal(0, 1),
                                     provsql.as_random(5)),
               provsql.categorical(ARRAY[0.5, 0.5], ARRAY[5.0, 6.0])),
             'independent') - 0.35) < 1e-12
       AS mixture_continuous_plus_dirac_vs_cat;

-- (F7) Independence guard: shared Bernoulli p_token makes the two
-- mixtures perfectly correlated through their selector, so the
-- sum-product factoring would silently mis-resolve.  RangeCheck must
-- bail and let the cmp flow through to the downstream path.  We
-- check this by verifying that 'independent' returns the same MC
-- estimate (within tolerance) as a direct 'monte-carlo' call -- a
-- silently-wrong shortcut would produce 0.4 · 0.4 = 0.16 instead of
-- the true 0.4.
DO $$
DECLARE
  p_tok uuid := public.uuid_generate_v4();
  lhs random_variable;
  rhs random_variable;
  p_independent double precision;
BEGIN
  PERFORM provsql.create_gate(p_tok, 'input');
  PERFORM provsql.set_prob(p_tok, 0.4);
  lhs := provsql.mixture(p_tok, provsql.as_random(0), provsql.as_random(1));
  rhs := provsql.mixture(p_tok, provsql.as_random(0), provsql.as_random(2));
  SET LOCAL provsql.monte_carlo_seed = 42;
  p_independent := provsql.probability_evaluate(
                     provsql.rv_cmp_eq(lhs, rhs), 'independent');
  /* True value is 0.4; MC is within ~1% over 10k samples (the hybrid
   * decomposer's default budget).  The silently-wrong shortcut would
   * have produced 0.16, far outside the tolerance.  We pin >= 0.30
   * (very loose) so this test is robust to MC seed changes. */
  IF p_independent < 0.30 OR p_independent > 0.50 THEN
    RAISE EXCEPTION 'independence guard failed: got %, expected ~0.4', p_independent;
  END IF;
END $$;
SELECT 't'::text AS shared_p_token_correlation_guard;

SELECT 'ok'::text AS continuous_rangecheck_done;
