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
-- Returns NULL for shapes V1 doesn't cover.  These are the cases
-- where the Studio frontend falls back to histogram-only rendering
-- (the overlay skip-test must pass without raising).
-- ---------------------------------------------------------------

-- (7) gate_arith composite root: N + U is not a single closed-form
-- family, so the function bails.
WITH r AS (SELECT provsql.normal(0, 1) + provsql.uniform(0, 1) AS s)
SELECT provsql.rv_analytical_curves((s)::uuid, 100)
       IS NULL AS arith_composite_returns_null
FROM r;

-- (8) Bernoulli mixture: V1 doesn't render the weighted-sum PDF yet.
WITH r AS (SELECT provsql.mixture(0.3, provsql.normal(0, 1),
                                        provsql.uniform(-1, 1)) AS m)
SELECT provsql.rv_analytical_curves((m)::uuid, 100)
       IS NULL AS mixture_returns_null
FROM r;

-- (9) Categorical: discrete distribution, V1 doesn't render a stem
-- plot yet.
WITH r AS (SELECT provsql.categorical(ARRAY[0.5, 0.5],
                                       ARRAY[0.0, 1.0]) AS c)
SELECT provsql.rv_analytical_curves((c)::uuid, 100)
       IS NULL AS categorical_returns_null
FROM r;

-- (10) Infeasible conditioning event: closed-form bails, returns NULL.
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
