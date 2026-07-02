\set ECHO none
\pset format unaligned

-- LogNormal(μ, σ) (§A.2): exp of a Normal(μ, σ), parameterised by the
-- underlying normal.  The multiplicative counterpart of Normal: the
-- family registers a product closure (parameters add in log space), the
-- exp/ln transform bridges to and from Normal, a comparator closed
-- form, closed-form (truncated) moments, and an exact quantile -- so
-- everything below runs with the MC fallback DISABLED, proving the
-- whole surface is analytic.

SET provsql.rv_mc_samples = 0;

-- (1) Closed-form moments: E = e^{μ+σ²/2}, Var = (e^{σ²}-1)e^{2μ+σ²},
-- E[X²] = e^{2μ+2σ²}.
SELECT abs(provsql.expected(provsql.lognormal(0, 1)) - exp(0.5)) < 1e-12
       AS lognormal_mean_exact;
SELECT abs(provsql.variance(provsql.lognormal(0, 1))
           - (exp(1.0) - 1) * exp(1.0)) < 1e-12 AS lognormal_var_exact;
SELECT abs(provsql.moment(provsql.lognormal(0.5, 2), 2)
           - exp(2 * 0.5 + 2 * 4)) < 1e-6 AS lognormal_m2_exact;

-- (2) CDF and support: P(X <= e^μ) = 1/2 (the median is e^μ);
-- P(LogN(0,1) <= e) = Φ(1).
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(provsql.lognormal(0, 1), 1::random_variable),
         'independent') = 0.5 AS lognormal_median_mass_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.lognormal(0, 1),
                               exp(1.0)::random_variable),
             'independent') - 0.8413447460685429) < 1e-12
       AS lognormal_cdf_exact;
SELECT lo, hi FROM support(provsql.lognormal(0, 1));

-- (3) Quantiles: median exactly e^μ; the 97.5% quantile is
-- exp(Φ⁻¹(0.975)) for LogN(0,1).
SELECT abs(provsql.quantile(provsql.lognormal(0, 1), 0.5) - 1) < 1e-12
       AS lognormal_median_exact;
SELECT abs(provsql.quantile(provsql.lognormal(0, 1), 0.975)
           - exp(1.959963984540054)) < 1e-10 AS lognormal_q975_exact;

-- (4) Comparator closed form: P(X < Y) for independent lognormals is
-- the underlying Normal comparison, Φ((μ_Y-μ_X)/√(σ_X²+σ_Y²)).
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(provsql.lognormal(0, 1),
                               provsql.lognormal(1, 1)),
             'independent') - 0.7602499389065233) < 1e-12
       AS lognormal_vs_lognormal_exact;

-- (5) The exp/ln transform bridges: exp(Normal(μ,σ)) IS LogNormal(μ,σ)
-- and ln(LogNormal(μ,σ)) is Normal(μ,σ).  The moment / quantile
-- evaluators consult the transform registry read-only (no rewrite, so
-- no shared-RV identity can be decoupled), exact with MC off...
SELECT abs(provsql.expected(exp(provsql.normal(0, 1))) - exp(0.5)) < 1e-12
       AS exp_normal_bridge_exact;
SELECT provsql.expected(ln(provsql.lognormal(2, 3))) = 2
       AS ln_lognormal_bridge_exact;
SELECT abs(provsql.quantile(exp(provsql.normal(0, 1)), 0.5) - 1) < 1e-12
       AS exp_normal_quantile_exact;
-- ...while the probability path runs the full simplifier, where chains
-- fold bottom-up: exp(N(0,1) + N(0,1)) = LogN(0, √2), whose mass below
-- 1 is exactly 1/2.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(exp(provsql.normal(0, 1) + provsql.normal(0, 1)),
                           1::random_variable),
         'independent') = 0.5 AS exp_normal_sum_chain_exact;

-- (6) Product closure: independent lognormals multiply to a lognormal
-- (parameters add in log space): LogN(0,1) · LogN(1,2) = LogN(1, √5),
-- mean e^{1+5/2} = e^3.5.
SELECT abs(provsql.expected(provsql.lognormal(0, 1) * provsql.lognormal(1, 2))
           - exp(3.5)) < 1e-12 AS lognormal_product_exact;
-- Positive scalars fold through affine: 2·LogN(0,1) = LogN(ln 2, 1).
SELECT abs(provsql.expected(2 * provsql.lognormal(0, 1)) - 2 * exp(0.5))
       < 1e-12 AS lognormal_scale_exact;
-- Scalar factors inside an n-ary product fold too: 3·(X·Y).
SELECT abs(provsql.expected(3 * (provsql.lognormal(0, 1)
                                 * provsql.lognormal(1, 2)))
           - 3 * exp(3.5)) < 1e-12 AS lognormal_scaled_product_exact;
-- Quantiles of a product go through the same closed-form image: the
-- median of LogN(1, √5) is e; and its variance is exact through the
-- disjoint-product decomposition, (e⁵-1)e⁷.
SELECT abs(provsql.quantile(provsql.lognormal(0, 1) * provsql.lognormal(1, 2),
                            0.5) - exp(1.0)) < 1e-12
       AS lognormal_product_median_exact;
SELECT abs(provsql.variance(provsql.lognormal(0, 1) * provsql.lognormal(1, 2))
           - (exp(5.0) - 1) * exp(7.0)) < 1e-6
       AS lognormal_product_var_exact;

-- (7) Conditioning: closed-form truncated moments in log space.
-- E[LogN(0,1) | X > 1] = e^{1/2}·Φ(1)/(1 - Φ(0)) ≈ 2.7742859576700096.
WITH r AS (SELECT provsql.lognormal(0, 1) AS x)
SELECT abs(provsql.expected(x | (x > 1)) - 2.7742859576700096) < 1e-12
       AS lognormal_truncated_mean_exact
  FROM r;

RESET provsql.rv_mc_samples;

-- (8) The sampler draws real lognormals (seeded MC agreement on a
-- statistic with no closed form registered: the mean of a lognormal
-- times an independent uniform).
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.expected(provsql.lognormal(0, 1) * provsql.uniform(0, 1))
           - exp(0.5) / 2) < 0.05 AS lognormal_mixed_product_mc;
RESET provsql.monte_carlo_seed;

-- (9) Constructor validation; σ=0 routes through as_random (a Dirac at
-- e^μ, sharing the constant's gate).
\set VERBOSITY terse
SELECT provsql.lognormal('NaN', 1);
SELECT provsql.lognormal(0, -1);
\set VERBOSITY default
SELECT (provsql.lognormal(1, 0))::uuid = (provsql.as_random(exp(1.0)))::uuid
       AS lognormal_sigma0_dedups;

-- (10) The family registry lists it (Studio renders it with no client
-- change).
SELECT name, nparams, param_names, label
  FROM provsql.rv_families() WHERE name = 'lognormal';

SELECT 'ok'::text AS continuous_lognormal_done;
