\set ECHO none
\pset format unaligned

-- Inverse-gamma(α, β) and inverse-Gaussian/Wald(μ, λ): the reciprocal of
-- a gamma (conjugate prior for a Gaussian variance) and the Brownian
-- first-passage time.  Both CDFs are closed form -- everything above the
-- RESET runs with the Monte-Carlo fallback disabled.

SET provsql.rv_mc_samples = 0;

-- ═══════════ Inverse gamma ═══════════

-- (1) Moments: E[IΓ(3,2)] = β/(α-1) = 1, Var = β²/((α-1)²(α-2)) = 1,
-- E[X²] = β²/((α-1)(α-2)) = 2.
SELECT provsql.expected(provsql.inverse_gamma(3, 2)) = 1 AS igamma_mean_exact;
SELECT provsql.variance(provsql.inverse_gamma(3, 2)) = 1 AS igamma_var_exact;
SELECT provsql.moment(provsql.inverse_gamma(3, 2), 2) = 2 AS igamma_m2_exact;

-- (2) Divergent moments reported honestly as Infinity, not estimated
-- (the mean for α ≤ 1, the variance for α ≤ 2).
SELECT provsql.expected(provsql.inverse_gamma(1, 2)) = 'Infinity'
       AS igamma_mean_divergent;
SELECT provsql.variance(provsql.inverse_gamma(2, 2)) = 'Infinity'
       AS igamma_var_divergent;

-- (3) CDF through the upper incomplete gamma: IΓ(1,1) has F(x) = e^{-1/x},
-- so P(X ≤ 1) = 1/e, exactly, and the numeric quantile inverts it.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.inverse_gamma(1, 1), 1::random_variable),
             'independent') - exp(-1.0)) < 1e-12
       AS igamma_cdf_exact;
SELECT abs(provsql.quantile(provsql.inverse_gamma(1, 1), exp(-1.0)) - 1)
       < 1e-9 AS igamma_quantile_numeric;
SELECT lo, hi FROM support(provsql.inverse_gamma(3, 2));

-- (4) Positive scaling rescales β: 2·IΓ(3,2) = IΓ(3,4), mean 4/2 = 2,
-- exact through the affine fold.
SELECT provsql.expected(2 * provsql.inverse_gamma(3, 2)) = 2
       AS igamma_scaled_mean_exact;

-- ═══════════ Inverse Gaussian (Wald) ═══════════

-- (5) Moments: E[IG(2,6)] = μ = 2, Var = μ³/λ = 4/3,
-- E[X²] = μ² + μ³/λ = 16/3.
SELECT provsql.expected(provsql.inverse_gaussian(2, 6)) = 2
       AS iwald_mean_exact;
SELECT abs(provsql.variance(provsql.inverse_gaussian(2, 6)) - 4.0 / 3) < 1e-12
       AS iwald_var_exact;
SELECT abs(provsql.moment(provsql.inverse_gaussian(2, 6), 2) - 16.0 / 3)
       < 1e-12 AS iwald_m2_exact;

-- (6) Closed-form CDF in Φ: P(IG(1,1) ≤ 1) = Φ(0) + e²Φ(-2)
-- ≈ 0.6681020012231706.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.inverse_gaussian(1, 1), 1::random_variable),
             'independent') - 0.6681020012231706) < 1e-12
       AS iwald_cdf_exact;
SELECT lo, hi FROM support(provsql.inverse_gaussian(2, 6));

-- (7) wald is an alias of inverse_gaussian.
SELECT provsql.expected(provsql.wald(2, 6)) = 2 AS wald_alias_mean_exact;
SELECT provsql.get_extra((provsql.wald(2, 6))::uuid) = 'inverse_gaussian:2,6'
       AS wald_alias_encoding;

-- (8) Same-ratio sum closure: IG(1,2) + IG(2,8) both have λ/μ² = 2, so
-- the sum folds to IG(3, 18) in the simplifier; its mean is 3 and its
-- CDF matches the folded single-RV CDF exactly (no MC noise).
SELECT provsql.expected(
         provsql.inverse_gaussian(1, 2) + provsql.inverse_gaussian(2, 8)) = 3
       AS iwald_sum_closure_mean;
WITH s AS (SELECT provsql.inverse_gaussian(1, 2)
                + provsql.inverse_gaussian(2, 8) AS x)
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(x, 3::random_variable), 'independent')
           - provsql.probability_evaluate(
               provsql.rv_cmp_le(provsql.inverse_gaussian(3, 18),
                                 3::random_variable), 'independent')) < 1e-12
       AS iwald_sum_closure_cdf
  FROM s;

-- (9) Positive scaling: 3·IG(2,6) = IG(6,18), mean 6.
SELECT provsql.expected(3 * provsql.inverse_gaussian(2, 6)) = 6
       AS iwald_scaled_mean_exact;

RESET provsql.rv_mc_samples;

-- (10) The samplers draw the real distributions (seeded MC agreement on
-- the mean of a product, for which no closed form is registered).
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.expected(provsql.inverse_gamma(4, 3) * provsql.uniform(0, 1))
           - 0.5) < 0.03 AS igamma_sampler_mc;
SELECT abs(provsql.expected(provsql.inverse_gaussian(2, 6) * provsql.uniform(0, 1))
           - 1.0) < 0.03 AS iwald_sampler_mc;
RESET provsql.monte_carlo_seed;

-- (11) End-to-end through the planner hook: WHERE rv <= c resolves to the
-- exact closed-form CDF answer.
CREATE TABLE inverse_sensors(id text, x provsql.random_variable);
INSERT INTO inverse_sensors VALUES
  ('a', provsql.inverse_gamma(1, 1)),
  ('b', provsql.inverse_gaussian(1, 1));
SELECT add_provenance('inverse_sensors');
CREATE TABLE inverse_result AS
  SELECT id, probability_evaluate(provenance(), 'independent') AS p
    FROM inverse_sensors WHERE x <= 1;
SELECT remove_provenance('inverse_result');
SELECT id,
       abs(p - CASE id WHEN 'a' THEN exp(-1.0)
                       WHEN 'b' THEN 0.6681020012231706 END) < 1e-12
       AS within_tolerance
  FROM inverse_result ORDER BY id;
DROP TABLE inverse_result;
DROP TABLE inverse_sensors;

-- (12) Constructor validation.
\set VERBOSITY terse
SELECT provsql.inverse_gamma(0, 1);
SELECT provsql.inverse_gamma(1, 'Infinity');
SELECT provsql.inverse_gaussian(-1, 2);
SELECT provsql.inverse_gaussian(1, 0);
SELECT provsql.wald(2, -1);
\set VERBOSITY default

-- (13) Both families are in the registry (Studio renders them with no
-- client change).
SELECT name, nparams, param_names, label
  FROM provsql.rv_families()
 WHERE name IN ('inverse_gamma', 'inverse_gaussian')
 ORDER BY name;

SELECT 'ok'::text AS continuous_inverse_done;
