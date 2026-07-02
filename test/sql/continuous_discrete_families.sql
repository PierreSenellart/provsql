\set ECHO none
\pset format unaligned

-- Discrete count families (§A.4): Poisson, Binomial, Geometric,
-- Hypergeometric, and negative binomial as convenience constructors
-- that enumerate a pmf (in log space, via categorical_from_log_pmf)
-- into the existing categorical gate -- no new gate machinery, and the
-- whole surface below runs with the MC fallback disabled: moments,
-- quantiles, and (in)equality comparisons are exact over the
-- enumerated support.

SET provsql.rv_mc_samples = 0;

-- (1) Poisson(4): mean and variance λ, cumulative P(X <= 3), median.
SELECT abs(provsql.expected(provsql.poisson(4)) - 4) < 1e-9
       AS poisson_mean_exact;
SELECT abs(provsql.variance(provsql.poisson(4)) - 4) < 1e-9
       AS poisson_var_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.poisson(4), 3::random_variable),
             'independent') - 0.43347012036670893) < 1e-9
       AS poisson_cdf_exact;
SELECT provsql.quantile(provsql.poisson(4), 0.5) = 4 AS poisson_median;
-- Large mean: the log-space recurrence survives where exp(-λ)
-- underflows (λ = 1000 has pmf(0) = e^-1000).
SELECT abs(provsql.expected(provsql.poisson(1000)) - 1000) < 1e-6
       AS poisson_large_lambda_stable;

-- (2) Binomial(10, 0.3): E = np, Var = np(1-p), and an exact EQUALITY
-- probability (discrete distributions decide = / <> exactly, unlike
-- the continuous P(X = c) = 0 rule): P(X = 3) = C(10,3)·0.3³·0.7⁷.
SELECT abs(provsql.expected(provsql.binomial(10, 0.3)) - 3) < 1e-12
       AS binomial_mean_exact;
SELECT abs(provsql.variance(provsql.binomial(10, 0.3)) - 2.1) < 1e-12
       AS binomial_var_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_eq(provsql.binomial(10, 0.3),
                               3::random_variable),
             'independent') - 0.266827932) < 1e-12
       AS binomial_point_mass_exact;

-- (3) Geometric(0.25), TRIALS convention (support starts at 1):
-- E = 1/p, Var = (1-p)/p², survival P(X > 4) = 0.75⁴.
SELECT abs(provsql.expected(provsql.geometric(0.25)) - 4) < 1e-9
       AS geometric_mean_exact;
SELECT abs(provsql.variance(provsql.geometric(0.25)) - 12) < 1e-8
       AS geometric_var_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_gt(provsql.geometric(0.25), 4::random_variable),
             'independent') - 0.31640625) < 1e-12
       AS geometric_survival_exact;

-- (4) Hypergeometric(20, 5, 4) -- 4 draws without replacement from 20
-- items of which 5 are marked: E = nK/N = 1,
-- Var = n(K/N)(1-K/N)(N-n)/(N-1) = 12/19, P(X = 0) = C(15,4)/C(20,4).
SELECT abs(provsql.expected(provsql.hypergeometric(20, 5, 4)) - 1) < 1e-12
       AS hypergeometric_mean_exact;
SELECT abs(provsql.variance(provsql.hypergeometric(20, 5, 4)) - 12.0 / 19)
       < 1e-12 AS hypergeometric_var_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_eq(provsql.hypergeometric(20, 5, 4),
                               0::random_variable),
             'independent') - 0.2817337461300310) < 1e-12
       AS hypergeometric_point_mass_exact;

-- (5) Negative binomial (failures before the r-th success), with real
-- r for overdispersed counts: E = r(1-p)/p, Var = r(1-p)/p².
SELECT abs(provsql.expected(provsql.negative_binomial(2.5, 0.5)) - 2.5)
       < 1e-9 AS negbin_mean_exact;
SELECT abs(provsql.variance(provsql.negative_binomial(2.5, 0.5)) - 5)
       < 1e-8 AS negbin_var_exact;

-- (6) The shared back end is directly usable for custom discrete pmfs
-- (unnormalised log-masses): a hand-rolled fair die.
SELECT abs(provsql.expected(provsql.categorical_from_log_pmf(
             ARRAY[1, 2, 3, 4, 5, 6]::float8[],
             ARRAY[0, 0, 0, 0, 0, 0]::float8[])) - 3.5) < 1e-12
       AS custom_log_pmf_exact;

-- (7) Degenerate parameters route through as_random (shared Dirac
-- gates).
SELECT (provsql.poisson(0))::uuid = (provsql.as_random(0))::uuid
       AS poisson_zero_dirac;
SELECT (provsql.binomial(7, 1))::uuid = (provsql.as_random(7))::uuid
       AS binomial_certain_dirac;
SELECT (provsql.geometric(1))::uuid = (provsql.as_random(1))::uuid
       AS geometric_certain_dirac;
SELECT (provsql.negative_binomial(3, 1))::uuid
       = (provsql.as_random(0))::uuid AS negbin_certain_dirac;

RESET provsql.rv_mc_samples;

-- (8) Validation and the support-size guard.
\set VERBOSITY terse
SELECT provsql.poisson(-1);
SELECT provsql.poisson(1e7);
SELECT provsql.binomial(-1, 0.5);
SELECT provsql.binomial(5, 1.5);
SELECT provsql.geometric(0);
SELECT provsql.hypergeometric(10, 12, 3);
SELECT provsql.negative_binomial(0, 0.5);
\set VERBOSITY default

SELECT 'ok'::text AS continuous_discrete_families_done;
