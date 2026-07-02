\set ECHO none
\pset format unaligned

-- Beta(α, β) (§A.3): the unit-interval family and conjugate prior of
-- Bernoulli/binomial success probabilities.  Moments, the incomplete-
-- beta CDF, bisection quantiles over the finite support, and truncated
-- moments are all analytic: everything above the RESET runs with the
-- MC fallback disabled.

SET provsql.rv_mc_samples = 0;

-- (1) Moments: E[Beta(2,3)] = 2/5, Var = 6/150 = 1/25,
-- E[X²] = (2·3)/(5·6) = 1/5.
SELECT provsql.expected(provsql.beta(2, 3)) = 0.4 AS beta_mean_exact;
SELECT abs(provsql.variance(provsql.beta(2, 3)) - 0.04) < 1e-15
       AS beta_var_exact;
SELECT abs(provsql.moment(provsql.beta(2, 3), 2) - 0.2) < 1e-15
       AS beta_m2_exact;

-- (2) CDF through the regularised incomplete beta:
-- I_0.5(2,2) = 1/2 (symmetry); I_0.3(2,3) = 0.3483 exactly (the
-- polynomial expansion terminates for integer shapes).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.beta(2, 2), 0.5::random_variable),
             'independent') - 0.5) < 1e-12 AS beta_symmetric_median;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.beta(2, 3), 0.3::random_variable),
             'independent') - 0.3483) < 1e-12 AS beta_cdf_exact;
SELECT lo, hi FROM support(provsql.beta(2, 3));

-- (3) Quantiles by CDF bisection over [0, 1]: the symmetric median,
-- and the round-trip of (2).
SELECT abs(provsql.quantile(provsql.beta(2, 2), 0.5) - 0.5) < 1e-9
       AS beta_median_exact;
SELECT abs(provsql.quantile(provsql.beta(2, 3), 0.3483) - 0.3) < 1e-9
       AS beta_quantile_roundtrip;

-- (4) Beta-vs-Beta comparison through the bounded-support quadrature
-- (smooth on [0,1] for shapes >= 1): P(Beta(2,2) < Beta(4,2)) = 5/7.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(provsql.beta(2, 2), provsql.beta(4, 2)),
             'independent') - 5.0 / 7) < 1e-6 AS beta_vs_beta_quadrature;

-- (5) Interval conditioning: closed-form truncated moments,
-- E[Beta(2,3) | X > 1/2] = 0.64 exactly.
WITH r AS (SELECT provsql.beta(2, 3) AS x)
SELECT abs(provsql.expected(x | (x > 0.5)) - 0.64) < 1e-12
       AS beta_truncated_mean_exact
  FROM r;

RESET provsql.rv_mc_samples;

-- (6) The gamma-ratio sampler draws real betas (seeded MC agreement on
-- a statistic with no closed form).
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.expected(provsql.beta(2, 3) * provsql.beta(3, 2))
           - 0.4 * 0.6) < 0.01 AS beta_sampler_mc;
RESET provsql.monte_carlo_seed;

-- (7) Beta(1,1) IS Uniform(0,1) and routes there.
SELECT provsql.get_extra((provsql.beta(1, 1))::uuid) = 'uniform:0,1'
       AS beta_uniform_routes;

-- (8) Validation.
\set VERBOSITY terse
SELECT provsql.beta(0, 1);
SELECT provsql.beta(2, -1);
SELECT provsql.beta('NaN', 2);
\set VERBOSITY default

-- (9) The family registry lists it (Studio renders it with no client
-- change).
SELECT name, nparams, param_names, label
  FROM provsql.rv_families() WHERE name = 'beta';

SELECT 'ok'::text AS continuous_beta_done;
