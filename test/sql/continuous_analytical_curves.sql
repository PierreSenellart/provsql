\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- provsql.rv_analytical_curves(token, samples, prov) returns a jsonb
-- {pdf: [{x, p}, ...], cdf: [{x, p}, ...]} payload with closed-form
-- density / cumulative samples for a bare gate_rv root.  Used by
-- ProvSQL Studio's Distribution profile to overlay the analytical
-- curve on the empirical histogram from rv_histogram.  Returns NULL
-- for shapes the closed-form table doesn't cover (gate_arith
-- composites, mixtures, categoricals, non-integer Erlang) so the
-- caller can dispatch in parallel with rv_histogram without a
-- structural pre-check.

-- ---------------------------------------------------------------
-- Bare gate_rv unconditional.  PDF at the mean / mode should match
-- the textbook value; total CDF over the natural support should
-- approach 1.
-- ---------------------------------------------------------------

-- (1) Normal(0, 1).  PDF at μ = 0 is 1/sqrt(2π) ≈ 0.3989.
--     The 100-sample window spans μ ± 4σ = [-4, 4], so the midpoint
--     sample is at x = 0 (50th index).
WITH r AS (SELECT provsql.normal(0, 1) AS n),
     c AS (SELECT provsql.rv_analytical_curves(
                    (n)::uuid, 101) AS j
             FROM r)
SELECT jsonb_array_length(j -> 'pdf') = 101
       AND jsonb_array_length(j -> 'cdf') = 101
       AND abs(((j -> 'pdf' -> 50 ->> 'x'))::float8 - 0.0) < 0.05
       AND abs(((j -> 'pdf' -> 50 ->> 'p'))::float8 - 0.3989422804014327) < 1e-3
       AND abs(((j -> 'cdf' -> 50 ->> 'p'))::float8 - 0.5) < 1e-3
       AS normal_unconditional
FROM c;

-- (2) Uniform(2, 5).  PDF = 1/3 ≈ 0.3333 over [2, 5]; the curve
--     window pads slightly outside the support so we check at an
--     interior sample.  CDF at the upper end approaches 1.
WITH r AS (SELECT provsql.uniform(2, 5) AS u),
     c AS (SELECT provsql.rv_analytical_curves(
                    (u)::uuid, 100) AS j
             FROM r)
SELECT jsonb_array_length(j -> 'pdf') = 100
       AND abs(((j -> 'pdf' -> 50 ->> 'p'))::float8 - (1.0/3.0)) < 1e-6
       AND ((j -> 'cdf' -> 99 ->> 'p'))::float8 = 1.0
       AS uniform_unconditional
FROM c;

-- (3) Exponential(0.5).  PDF at x = 0 is λ = 0.5; CDF at x = 0 is 0.
WITH r AS (SELECT provsql.exponential(0.5) AS e),
     c AS (SELECT provsql.rv_analytical_curves(
                    (e)::uuid, 100) AS j
             FROM r)
SELECT jsonb_array_length(j -> 'pdf') = 100
       AND ((j -> 'pdf' -> 0 ->> 'x'))::float8 = 0.0
       AND abs(((j -> 'pdf' -> 0 ->> 'p'))::float8 - 0.5) < 1e-9
       AND ((j -> 'cdf' -> 0 ->> 'p'))::float8 = 0.0
       AS exponential_unconditional
FROM c;

-- (4) Erlang(3, 1).  PDF at x = 0 is 0 (k > 1 shape); CDF at x = 0 is 0.
WITH r AS (SELECT provsql.erlang(3, 1) AS er),
     c AS (SELECT provsql.rv_analytical_curves(
                    (er)::uuid, 100) AS j
             FROM r)
SELECT jsonb_array_length(j -> 'pdf') = 100
       AND ((j -> 'pdf' -> 0 ->> 'p'))::float8 = 0.0
       AND ((j -> 'cdf' -> 0 ->> 'p'))::float8 = 0.0
       AS erlang_unconditional
FROM c;

-- ---------------------------------------------------------------
-- Truncated single-RV.  The PDF is the unconditional PDF over the
-- truncation, normalised by Z = Phi(hi) - Phi(lo); the CDF is the
-- unconditional CDF rescaled to [0, 1] over the truncation.  The
-- curve's x-range collapses to the truncation interval; the first
-- and last samples sit at the truncation bounds.
-- ---------------------------------------------------------------

-- (5) Normal(0, 1) | -2 < X < 2.  Z = Phi(2) - Phi(-2) ≈ 0.9545.
--     Truncated PDF at the midpoint (x = 0) = phi(0) / Z ≈ 0.4180.
--     CDF at the upper bound is 1 by construction (normalised).
WITH r AS (SELECT provsql.normal(0, 1) AS n),
     ev AS (SELECT provsql.provenance_times(
                     provsql.rv_cmp_gt(n, -2::random_variable),
                     provsql.rv_cmp_lt(n,  2::random_variable)) AS ev,
                   (n)::uuid AS tok FROM r),
     c AS (SELECT provsql.rv_analytical_curves(tok, 101, ev) AS j FROM ev)
SELECT jsonb_array_length(j -> 'pdf') = 101
       AND abs(((j -> 'pdf' -> 0  ->> 'x'))::float8 - (-2.0)) < 1e-9
       AND abs(((j -> 'pdf' -> 100 ->> 'x'))::float8 - 2.0)   < 1e-9
       AND abs(((j -> 'pdf' -> 50 ->> 'p'))::float8 - 0.4180232480629079) < 1e-3
       AND abs(((j -> 'cdf' -> 100 ->> 'p'))::float8 - 1.0) < 1e-9
       AND abs(((j -> 'cdf' -> 0   ->> 'p'))::float8 - 0.0) < 1e-9
       AS truncated_normal
FROM c;

-- (6) Uniform(0, 10) | X > 9.5.  Truncation -> U(9.5, 10) flat PDF
--     at height 1/0.5 = 2.0; CDF runs from 0 to 1 over [9.5, 10].
WITH r AS (SELECT provsql.uniform(0, 10) AS u),
     ev AS (SELECT provsql.rv_cmp_gt(u, 9.5::random_variable) AS ev,
                   (u)::uuid AS tok FROM r),
     c AS (SELECT provsql.rv_analytical_curves(tok, 100, ev) AS j FROM ev)
SELECT jsonb_array_length(j -> 'pdf') = 100
       AND abs(((j -> 'pdf' -> 50 ->> 'p'))::float8 - 2.0) < 1e-9
       AND abs(((j -> 'cdf' -> 0  ->> 'p'))::float8 - 0.0) < 1e-9
       AND abs(((j -> 'cdf' -> 99 ->> 'p'))::float8 - 1.0) < 1e-9
       AS truncated_uniform
FROM c;

-- ---------------------------------------------------------------
-- Bernoulli mixtures over closed-form arms: the analytical PDF is
-- the weighted sum of the children's PDFs; the x-range covers the
-- union of branch supports.  Dirac arms contribute point masses
-- under the `stems` field, with weight equal to the parent's
-- Bernoulli weight (and recursively for nested mixtures).
-- ---------------------------------------------------------------

-- (7) Bimodal: mixture(0.3, Normal(0, 1), Normal(10, 1)).  Two
-- local maxima at x ≈ 0 (height ≈ 0.3 · φ(0) ≈ 0.1197) and
-- x ≈ 10 (height ≈ 0.7 · φ(0) ≈ 0.2793).  No stems.
WITH r AS (SELECT provsql.mixture(0.3, provsql.normal(0, 1),
                                        provsql.normal(10, 1)) AS m),
     c AS (SELECT provsql.rv_analytical_curves((m)::uuid, 201) AS j FROM r),
     pdf AS (SELECT (e ->> 'x')::float8 AS x,
                    (e ->> 'p')::float8 AS p
               FROM c, jsonb_array_elements(j -> 'pdf') AS e)
SELECT abs(MAX(p) FILTER (WHERE x < 5.0) - 0.3 * 0.3989422804014327) < 5e-3
       AND abs(MAX(p) FILTER (WHERE x > 5.0) - 0.7 * 0.3989422804014327) < 5e-3
       AND (SELECT (j -> 'stems') IS NULL FROM c) AS mixture_bimodal_normal
FROM pdf;

-- (8) Mixed continuous + Dirac: mixture(0.4, Uniform(0, 1), as_random(5)).
-- The continuous part is a flat 0.4 over [0, 1]; the Dirac at 5
-- becomes a stem of weight 0.6.  pdf and stems both present.
WITH r AS (SELECT provsql.mixture(0.4, provsql.uniform(0, 1),
                                        provsql.as_random(5)) AS m),
     c AS (SELECT provsql.rv_analytical_curves((m)::uuid, 201) AS j FROM r)
SELECT (j -> 'pdf') IS NOT NULL
       AND jsonb_array_length(j -> 'stems') = 1
       AND abs(((j -> 'stems' -> 0 ->> 'x'))::float8 - 5.0) < 1e-9
       AND abs(((j -> 'stems' -> 0 ->> 'p'))::float8 - 0.6) < 1e-9
       AS mixture_continuous_plus_dirac
FROM c;

-- (9) Pure Dirac root via as_random: a single stem at the constant,
-- no continuous pdf, and a CDF staircase that jumps from 0 to 1 at
-- the constant value (samples before the jump are 0, after are 1).
WITH r AS (SELECT provsql.as_random(3.5) AS d),
     c AS (SELECT provsql.rv_analytical_curves((d)::uuid, 100) AS j FROM r)
SELECT (j -> 'pdf') IS NULL
       AND jsonb_array_length(j -> 'cdf') = 100
       AND jsonb_array_length(j -> 'stems') = 1
       AND abs(((j -> 'stems' -> 0 ->> 'x'))::float8 - 3.5) < 1e-9
       AND abs(((j -> 'stems' -> 0 ->> 'p'))::float8 - 1.0) < 1e-9
       AND ((j -> 'cdf' -> 0  ->> 'p'))::float8 = 0.0
       AND ((j -> 'cdf' -> 99 ->> 'p'))::float8 = 1.0
       AS dirac_unconditional
FROM c;

-- (10) Categorical: three outcomes with prescribed masses.  Stems
-- match the constructor arguments exactly; CDF staircase ends at 1
-- and the cumulative jump before x = 0 equals the first outcome's
-- mass (0.3).
WITH r AS (SELECT provsql.categorical(ARRAY[0.3, 0.5, 0.2],
                                       ARRAY[-1.0, 0.0, 2.0]) AS c),
     c AS (SELECT provsql.rv_analytical_curves((c)::uuid, 100) AS j FROM r),
     st AS (SELECT (e ->> 'x')::float8 AS x,
                   (e ->> 'p')::float8 AS p
              FROM c, jsonb_array_elements(j -> 'stems') AS e)
SELECT (SELECT (j -> 'pdf') IS NULL FROM c)
       AND (SELECT abs(((j -> 'cdf' -> 99 ->> 'p'))::float8 - 1.0) < 1e-9 FROM c)
       AND COUNT(*) = 3
       AND abs(SUM(p) - 1.0) < 1e-9
       AND abs(MAX(p) FILTER (WHERE x = -1.0) - 0.3) < 1e-9
       AND abs(MAX(p) FILTER (WHERE x =  0.0) - 0.5) < 1e-9
       AND abs(MAX(p) FILTER (WHERE x =  2.0) - 0.2) < 1e-9
       AS categorical_three_outcomes
FROM st;

-- ---------------------------------------------------------------
-- Returns NULL for shapes the closed-form table doesn't cover.
-- These are the cases where the Studio frontend falls back to
-- histogram-only rendering.
-- ---------------------------------------------------------------

-- (11) gate_arith composite root: N + U is not a single closed-form
-- family, so the function bails.
WITH r AS (SELECT provsql.normal(0, 1) + provsql.uniform(0, 1) AS s)
SELECT provsql.rv_analytical_curves((s)::uuid, 100)
       IS NULL AS arith_composite_returns_null
FROM r;

-- (11b) Hybrid-simplifier integration.  c * Exp(λ) is a gate_arith
-- composite at persistence time, but the simplifier folds it to a
-- single Exp(λ/c) leaf.  rv_analytical_curves must run the same
-- simplifier so the closed-form path picks up the folded shape; a
-- regression where the simplifier is bypassed would surface as a
-- NULL payload here.
--   2 * Exp(1)  ≡  Exp(0.5):
--     pdf(0)            = 0.5
--     pdf(ln 2 / 0.5)   = 0.5 * exp(-ln 2) = 0.25 at x ≈ 1.386
WITH r AS (SELECT 2 * provsql.exponential(1.0) AS s),
     c AS (SELECT provsql.rv_analytical_curves((s)::uuid, 100) AS j FROM r)
SELECT (j -> 'pdf') IS NOT NULL
       AND abs(((j -> 'pdf' -> 0 ->> 'x'))::float8 - 0.0) < 1e-9
       AND abs(((j -> 'pdf' -> 0 ->> 'p'))::float8 - 0.5) < 1e-9
       AND abs(((j -> 'cdf' -> 0 ->> 'p'))::float8 - 0.0) < 1e-9
       AS hybrid_simplifier_fold_picked_up
FROM c;

-- (11c) Independent event leaves the mixture unchanged.
-- collectRvConstraints finds no cmp constraining the mixture
-- variable (the event is on `aux`, a different RV); truncateShape
-- with the mixture's full support is the identity.  The bimodal
-- curve must look the same as the unconditional case.
WITH r AS (SELECT provsql.mixture(0.3, provsql.normal(0, 1),
                                        provsql.normal(10, 1)) AS m,
                  provsql.normal(5, 1) AS aux),
     ev AS (SELECT (m)::uuid AS tok,
                   provsql.rv_cmp_gt(aux, 5::random_variable) AS ev
              FROM r),
     c AS (SELECT provsql.rv_analytical_curves(tok, 201, ev) AS j FROM ev),
     pdf AS (SELECT (e ->> 'x')::float8 AS x,
                    (e ->> 'p')::float8 AS p
               FROM c, jsonb_array_elements(j -> 'pdf') AS e)
SELECT abs(MAX(p) FILTER (WHERE x < 5.0) - 0.3 * 0.3989422804014327) < 5e-3
       AND abs(MAX(p) FILTER (WHERE x > 5.0) - 0.7 * 0.3989422804014327) < 5e-3
       AS independent_event_leaves_mixture_unchanged
FROM pdf;

-- (11d) Truncated mixture on the mixture variable itself.  Both
-- arms get clipped to (x > 5); the Bernoulli weight reweights by
-- the ratio of arm masses (the truncated_normal_left_arm has tiny
-- mass to the right of 5, so the rebalanced p is dominated by the
-- right arm's mass — almost a pure Normal(10,1) restricted to x>5).
WITH r AS (SELECT provsql.mixture(0.3, provsql.normal(0, 1),
                                        provsql.normal(10, 1)) AS m),
     ev AS (SELECT (m)::uuid AS tok,
                   provsql.rv_cmp_gt(m, 5::random_variable) AS ev
              FROM r),
     c AS (SELECT provsql.rv_analytical_curves(tok, 201, ev) AS j FROM ev),
     pdf AS (SELECT (e ->> 'x')::float8 AS x,
                    (e ->> 'p')::float8 AS p
               FROM c, jsonb_array_elements(j -> 'pdf') AS e),
     peak AS (SELECT MAX(p) AS peak_p FROM pdf)
SELECT (SELECT (j -> 'pdf') IS NOT NULL FROM c)
       -- First sample at x = 5 (truncation boundary).
       AND abs((SELECT x FROM pdf ORDER BY x LIMIT 1) - 5.0) < 1e-9
       -- Right tail at x = 10 (peak of the right arm): peak ≈
       -- φ(0) / Z ≈ 0.3989 with Z very close to 1.
       AND abs(peak_p - 0.3989422804014327) < 5e-3
       AS truncated_mixture_clipped_to_right_arm
FROM peak;

-- (11e) Truncated categorical on the categorical variable: outcomes
-- outside the event interval are dropped; surviving outcomes have
-- their masses renormalised to sum to 1.  3-outcome categorical
-- under c > 0 keeps only the third outcome (value = 2).
-- Use a strict cutoff away from any outcome so the strict-vs-
-- non-strict collapse in intersectRvConstraint (which closes
-- strict inequalities for the continuous-support setting) doesn't
-- accidentally keep an outcome lying on the boundary.
WITH r AS (SELECT provsql.categorical(ARRAY[0.3, 0.5, 0.2],
                                       ARRAY[-1.0, 0.0, 2.0]) AS c),
     ev AS (SELECT (c)::uuid AS tok,
                   provsql.rv_cmp_gt(c, 1::random_variable) AS ev
              FROM r),
     j AS (SELECT provsql.rv_analytical_curves(tok, 100, ev) AS jj FROM ev)
SELECT (jj -> 'pdf') IS NULL
       AND jsonb_array_length(jj -> 'stems') = 1
       AND abs(((jj -> 'stems' -> 0 ->> 'x'))::float8 - 2.0) < 1e-9
       AND abs(((jj -> 'stems' -> 0 ->> 'p'))::float8 - 1.0) < 1e-9
       AS truncated_categorical_drops_outcomes
FROM j;

-- (11f) Truncated Dirac, feasible: as_random(5) restricted to x < 10
-- keeps the point; mass remains 1.
WITH r AS (SELECT provsql.as_random(5) AS d),
     ev AS (SELECT (d)::uuid AS tok,
                   provsql.rv_cmp_lt(d, 10::random_variable) AS ev
              FROM r),
     j AS (SELECT provsql.rv_analytical_curves(tok, 100, ev) AS jj FROM ev)
SELECT (jj -> 'pdf') IS NULL
       AND jsonb_array_length(jj -> 'stems') = 1
       AND abs(((jj -> 'stems' -> 0 ->> 'x'))::float8 - 5.0) < 1e-9
       AND abs(((jj -> 'stems' -> 0 ->> 'p'))::float8 - 1.0) < 1e-9
       AS truncated_dirac_feasible
FROM j;

-- (11e) Nested mixture with a Dirac arm.  The recursive matcher must
-- propagate Bernoulli weights along the path: a Dirac at 10 sitting
-- inside a (0.5, _, (0.8, _, dirac)) tree carries weight 0.5·0.2 = 0.1,
-- the rest is continuous PDF over Normal(0,1) (0.5) + Normal(5,1)
-- (0.5·0.8 = 0.4).  Use 201 samples so a sample lands close to each
-- mode (x = 0 and x = 5).
WITH r AS (SELECT provsql.mixture(0.5,
                          provsql.normal(0, 1),
                          provsql.mixture(0.8,
                                  provsql.normal(5, 1),
                                  provsql.as_random(10))) AS m),
     c AS (SELECT provsql.rv_analytical_curves((m)::uuid, 201) AS j FROM r)
SELECT (j -> 'pdf') IS NOT NULL
       AND jsonb_array_length(j -> 'stems') = 1
       AND abs(((j -> 'stems' -> 0 ->> 'x'))::float8 - 10.0) < 1e-9
       -- Nested weight: top mixture's right arm (0.5), inner mixture's
       -- right arm (0.2) → 0.5 · 0.2 = 0.1.
       AND abs(((j -> 'stems' -> 0 ->> 'p'))::float8 - 0.1) < 1e-9
       AS nested_mixture_with_dirac
FROM c;

-- (12) Infeasible conditioning event: closed-form bails, returns NULL.
-- Uniform(0, 10) | X > 100 collapses to an empty support; the curve
-- function returns NULL so the panel falls back to whatever
-- rv_histogram produced (which itself errors on infeasible events
-- under tight MC budgets).
WITH r AS (SELECT provsql.uniform(0, 10) AS u),
     ev AS (SELECT provsql.rv_cmp_gt(u, 100::random_variable) AS ev,
                   (u)::uuid AS tok FROM r)
SELECT provsql.rv_analytical_curves(tok, 100, ev) IS NULL
       AS infeasible_event_returns_null
FROM ev;

SELECT 'ok'::text AS continuous_analytical_curves_done;
