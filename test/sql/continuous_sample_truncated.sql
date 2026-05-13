\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Truncated-distribution closed-form fast path in rv_sample / rv_histogram.
--
-- The two SRFs route the conditional case (prov != gate_one()) through
-- the truncated-distribution closed-form sampler when the root is a
-- bare gate_rv of a supported family (Uniform / Normal / Exponential)
-- and the event reduces to a single interval on it.  Tight events that
-- previously degraded the MC-rejection budget now produce exactly N
-- samples with 100% acceptance.  Erlang and non-bare-gate_rv shapes
-- fall through to the MC rejection path unchanged.
--
-- Tests pin provsql.monte_carlo_seed so the empirical moments below
-- are deterministic across runs.  The closed-form path is exact in
-- distribution; the empirical-moment assertions use a tolerance scaled
-- to the n=10000 sampling noise (~1/sqrt(n) on the mean, ~1/sqrt(n/2)
-- on the variance).
SET provsql.monte_carlo_seed = 42;

-- ---------------------------------------------------------------
-- Uniform truncation.  U(0, 10) | X > 9.5 -> U(9.5, 10).
-- collectRvConstraints intersects with the RV's natural support, so a
-- plain U(9.5, 10) draw is the conditional distribution; 100%
-- acceptance.  Theoretical mean = 9.75, variance = 1/48 = 0.02083.
-- ---------------------------------------------------------------
WITH r AS (SELECT provsql.uniform(0, 10) AS u),
     ev AS (SELECT provsql.rv_cmp_gt(u, 9.5::random_variable) AS ev,
                   (u)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) = 10000
       AND count(*) FILTER (WHERE v BETWEEN 9.5 AND 10) = 10000
       AND abs(avg(v) - 9.75) < 0.01
       AND abs(var_samp(v) - 0.02083) < 0.005
       AS uniform_truncation_exact
FROM s;

-- ---------------------------------------------------------------
-- Exponential one-sided.  Exp(0.4) | X > 5 = 5 + Exp(0.4) by
-- memorylessness.  Theoretical mean = 5 + 1/0.4 = 7.5, var = 1/0.16
-- = 6.25.  100% acceptance via the memorylessness shortcut.
-- ---------------------------------------------------------------
WITH r AS (SELECT provsql.exponential(0.4) AS e),
     ev AS (SELECT provsql.rv_cmp_gt(e, 5::random_variable) AS ev,
                   (e)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) = 10000
       AND count(*) FILTER (WHERE v >= 5) = 10000
       AND abs(avg(v) - 7.5) < 0.1
       AND abs(var_samp(v) - 6.25) < 0.3
       AS exponential_memorylessness_exact
FROM s;

-- ---------------------------------------------------------------
-- Exponential two-sided.  Exp(1) | 1 < X < 3 -> truncated exponential
-- via inverse-CDF (std::log1p / std::expm1 for numerical accuracy).
-- Truncated mean = (F'(a) - F'(b)) / (F(b) - F(a)) - actually:
--   E[X | a < X < b] for Exp(λ) =
--     [exp(-λa)(λa + 1) - exp(-λb)(λb + 1)] / [λ (exp(-λa) - exp(-λb))]
-- For λ=1, a=1, b=3:
--   numerator = e^-1 * (1+1) - e^-3 * (3+1) = 2/e - 4/e^3 ≈ 0.5364
--   denominator = e^-1 - e^-3 ≈ 0.3181
--   mean ≈ 1.6863.
-- ---------------------------------------------------------------
WITH r AS (SELECT provsql.exponential(1) AS e),
     ev AS (SELECT provsql.provenance_times(
                     provsql.rv_cmp_gt(e, 1::random_variable),
                     provsql.rv_cmp_lt(e, 3::random_variable)) AS ev,
                   (e)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) = 10000
       AND count(*) FILTER (WHERE v >= 1 AND v <= 3) = 10000
       AND abs(avg(v) - 1.6863) < 0.05
       AS exponential_two_sided_inverse_cdf
FROM s;

-- ---------------------------------------------------------------
-- Normal two-sided.  N(0, 1) | -2 < X < 2 via inverse-CDF transform.
-- The forward CDF uses std::erf (same kernel as AnalyticEvaluator),
-- the inverse uses the Beasley-Springer-Moro approximation.
-- Theoretical truncated moments:
--   Z = Phi(2) - Phi(-2) ≈ 0.9545
--   mean = 0 (symmetric)
--   variance = 1 - 2 phi(2) / Phi(2,-2) * (2 - (-2)) / 2 ≈ 0.7737
-- Note: this case ALSO exercises the load-time @c runConstantFold
-- pass, which lifts the `-2::random_variable` parser shape
-- (gate_arith NEG over gate_value:2) into a clean gate_value:-2 so
-- @c collectRvConstraints recognises the cmp's constant side.
-- ---------------------------------------------------------------
WITH r AS (SELECT provsql.normal(0, 1) AS n),
     ev AS (SELECT provsql.provenance_times(
                     provsql.rv_cmp_gt(n, -2::random_variable),
                     provsql.rv_cmp_lt(n,  2::random_variable)) AS ev,
                   (n)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) = 10000
       AND count(*) FILTER (WHERE v >= -2 AND v <= 2) = 10000
       AND abs(avg(v)) < 0.05
       AND abs(var_samp(v) - 0.7737) < 0.05
       AS normal_two_sided_inverse_cdf
FROM s;

-- ---------------------------------------------------------------
-- Asymmetric Normal truncation: N(0, 1) | 1 < X < 3.
-- Theoretical mean of truncated normal:
--   mean = mu + sigma * (phi(alpha) - phi(beta)) / (Phi(beta) - Phi(alpha))
--   alpha = 1, beta = 3
--   phi(1) ≈ 0.24197, phi(3) ≈ 0.00443
--   Phi(1) ≈ 0.84134, Phi(3) ≈ 0.99865
--   mean ≈ 0 + 1 * (0.24197 - 0.00443) / (0.99865 - 0.84134) ≈ 1.5098
-- Strong validation of inv_phi: a sign-flipped or constant-broken BSM
-- would shift this mean by O(0.5) or more, far outside the tolerance.
-- ---------------------------------------------------------------
WITH r AS (SELECT provsql.normal(0, 1) AS n),
     ev AS (SELECT provsql.provenance_times(
                     provsql.rv_cmp_gt(n, 1::random_variable),
                     provsql.rv_cmp_lt(n, 3::random_variable)) AS ev,
                   (n)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) = 10000
       AND count(*) FILTER (WHERE v >= 1 AND v <= 3) = 10000
       AND abs(avg(v) - 1.5098) < 0.05
       AS normal_asymmetric_truncation
FROM s;

-- ---------------------------------------------------------------
-- Erlang truncation falls through to MC rejection.  Erlang(2, 1) | X > 3.
-- P(X > 3) for Erlang(2, 1) = (1 + 3) * e^-3 ≈ 0.1991, so ~20%
-- acceptance.  We don't pin the count (depends on budget) -- only check
-- that all returned samples are >= 3 and at least some are returned
-- (i.e., the MC fallback ran with a positive acceptance rate).
-- ---------------------------------------------------------------
SET provsql.rv_mc_samples = 100000;   -- enough budget to deliver 10k
\set VERBOSITY terse
WITH r AS (SELECT provsql.erlang(2, 1) AS e),
     ev AS (SELECT provsql.rv_cmp_gt(e, 3::random_variable) AS ev,
                   (e)::uuid AS tok
              FROM r),
     s AS (SELECT v FROM ev, provsql.rv_sample(tok, 10000, ev) v)
SELECT count(*) >= 9000   -- ~10000 expected with 20% acceptance on 100k budget
       AND count(*) FILTER (WHERE v >= 3) = count(*)
       AS erlang_falls_through_to_mc
FROM s;
\set VERBOSITY default
RESET provsql.rv_mc_samples;

-- ---------------------------------------------------------------
-- rv_histogram regression: tight provsql.rv_mc_samples budget that
-- previously caused "conditional MC accepted 0 of N samples" no longer
-- raises on closed-form-handled shapes.  Truncated U(0, 100) | X > 99
-- has 1% acceptance via rejection, but the closed-form path produces
-- exactly N samples.  Pin rv_mc_samples = 100 so the MC fallback would
-- have failed; verify the histogram succeeds and is bounded in [99, 100].
-- ---------------------------------------------------------------
SET provsql.rv_mc_samples = 100;
WITH r AS (SELECT provsql.uniform(0, 100) AS u),
     ev AS (SELECT provsql.rv_cmp_gt(u, 99::random_variable) AS ev,
                   (u)::uuid AS tok
              FROM r),
     h AS (SELECT provsql.rv_histogram(tok, 5, ev)::jsonb AS j FROM ev)
SELECT jsonb_array_length(j) = 5
       AND (j -> 0 ->> 'bin_lo')::float8 = 99.0
       AND (j -> 4 ->> 'bin_hi')::float8 = 100.0
       AS histogram_tight_budget_closed_form
FROM h;
RESET provsql.rv_mc_samples;

RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS continuous_sample_truncated_done;
