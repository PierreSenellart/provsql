\set ECHO none
\pset format unaligned

-- Weibull(k, λ) (§3.4) and Pareto(xₘ, α) (§3.5): the reliability /
-- survival family and the heavy-tailed power law.  Both surfaces are
-- analytic -- everything below the RESET runs with the MC fallback
-- disabled.

SET provsql.rv_mc_samples = 0;

-- ═══════════ Weibull ═══════════

-- (1) Moments: E[W(2,3)] = 3Γ(3/2) = 3√π/2; Var = 9(1 - π/4).
SELECT abs(provsql.expected(provsql.weibull(2, 3)) - 3 * sqrt(pi()) / 2)
       < 1e-12 AS weibull_mean_exact;
SELECT abs(provsql.variance(provsql.weibull(2, 3)) - 9 * (1 - pi() / 4))
       < 1e-12 AS weibull_var_exact;

-- (2) CDF and quantile: F(λ) = 1 - e⁻¹ at the scale, exactly inverted.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_le(provsql.weibull(2, 3), 3::random_variable),
             'independent') - (1 - exp(-1.0))) < 1e-15
       AS weibull_cdf_exact;
SELECT abs(provsql.quantile(provsql.weibull(2, 3), 1 - exp(-1.0)) - 3)
       < 1e-12 AS weibull_quantile_exact;
SELECT lo, hi FROM support(provsql.weibull(2, 3));

-- (3) Same-shape comparator closed form:
-- P(W(2,1) < W(2,2)) = λ_Y²/(λ_X² + λ_Y²) = 4/5.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_lt(provsql.weibull(2, 1), provsql.weibull(2, 2)),
         'independent') = 0.8 AS weibull_same_shape_exact;

-- (4) Truncated moments through the incomplete gamma:
-- E[W(2,1) | X > 1] ≈ 1.3789360780706561.
WITH r AS (SELECT provsql.weibull(2, 1) AS x)
SELECT abs(provsql.expected(x | (x > 1)) - 1.3789360780706561) < 1e-12
       AS weibull_truncated_mean_exact
  FROM r;

-- (5) Min-stability order statistic: the min of two i.i.d. W(2,3) is
-- W(2, 3/√2), mean 3√π/(2√2).
WITH r AS (SELECT provsql.weibull(2, 3) AS x, provsql.weibull(2, 3) AS y)
SELECT abs(provsql.expected(provsql.least(x, y))
           - 3 * sqrt(pi()) / (2 * sqrt(2.0))) < 1e-12
       AS weibull_min_exact
  FROM r;

-- (6) Positive scalings rescale λ: quantile(2·W(2,3), p) is exact
-- through the affine image.
SELECT abs(provsql.quantile(2 * provsql.weibull(2, 3), 1 - exp(-1.0)) - 6)
       < 1e-12 AS weibull_scaled_quantile_exact;

-- (7) k = 1 routes through exponential (W(1, λ) IS Exp(1/λ)): the
-- minted gate carries the exponential encoding, not a weibull one.
SELECT provsql.get_extra((provsql.weibull(1, 2))::uuid)
       = 'exponential:0.5' AS weibull_k1_routes_to_exponential;

-- ═══════════ Pareto ═══════════

-- (8) Moments: E[Par(1,3)] = 3/2, Var = 3/4; divergent moments are
-- reported as Infinity, not estimated.
SELECT provsql.expected(provsql.pareto(1, 3)) = 1.5 AS pareto_mean_exact;
SELECT provsql.variance(provsql.pareto(1, 3)) = 0.75 AS pareto_var_exact;
SELECT provsql.expected(provsql.pareto(1, 1)) = 'Infinity'
       AS pareto_mean_divergent;
SELECT provsql.variance(provsql.pareto(1, 2)) = 'Infinity'
       AS pareto_var_divergent;

-- (9) CDF, quantile, support: F(2) = 3/4 for Par(1,2), exactly inverted.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_le(provsql.pareto(1, 2), 2::random_variable),
         'independent') = 0.75 AS pareto_cdf_exact;
SELECT provsql.quantile(provsql.pareto(1, 2), 0.75) = 2
       AS pareto_quantile_exact;
SELECT lo, hi FROM support(provsql.pareto(2, 1));

-- (10) Comparator closed form, exact for ANY parameters (the pair the
-- heavy-tailed quadrature handles worst): same scale reduces to the
-- exponential ratio, P(Par(1,1) < Par(1,3)) = 1/4; across scales,
-- P(Par(1,2) < Par(2,1)) = 1 - (1/3)(1/2)² = 11/12.
SELECT provsql.probability_evaluate(
         provsql.rv_cmp_lt(provsql.pareto(1, 1), provsql.pareto(1, 3)),
         'independent') = 0.25 AS pareto_same_scale_exact;
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(provsql.pareto(1, 2), provsql.pareto(2, 1)),
             'independent') - 11.0 / 12) < 1e-15
       AS pareto_cross_scale_exact;

-- A mixed-family pair is a registry miss and routes through the
-- generic quadrature, which must integrate over the NARROWER window:
-- the Pareto's own power-law window is xₘ·10^{9/α}, far too wide for
-- the fixed panel budget, while the other side's window resolves fine
-- against the Pareto's exact CDF.
-- P(Par(1,3) < U(2,4)) = 1 - (2⁻² - 4⁻²)/((3-1)·(4-2)) = 61/64.
SELECT abs(provsql.probability_evaluate(
             provsql.rv_cmp_lt(provsql.pareto(1, 3), provsql.uniform(2, 4)),
             'independent') - 61.0 / 64) < 1e-9
       AS pareto_mixed_quadrature_exact;

-- (11) Self-similarity: X | X > a is Pareto(a, α), so the truncated
-- mean E[Par(1,3) | X > 2] is the Pareto(2,3) mean, 3.
WITH r AS (SELECT provsql.pareto(1, 3) AS x)
SELECT provsql.expected(x | (x > 2)) = 3 AS pareto_truncated_mean_exact
  FROM r;

-- (12) Min-stability: the min of two i.i.d. Par(1,3) is Par(1,6),
-- mean 6/5.
WITH r AS (SELECT provsql.pareto(1, 3) AS x, provsql.pareto(1, 3) AS y)
SELECT provsql.expected(provsql.least(x, y)) = 1.2 AS pareto_min_exact
  FROM r;

RESET provsql.rv_mc_samples;

-- (13) The samplers draw the real distributions (seeded MC agreement on
-- statistics with no closed form registered).
SET provsql.monte_carlo_seed = 42;
SELECT abs(provsql.expected(provsql.weibull(2, 1) * provsql.uniform(0, 1))
           - sqrt(pi()) / 4) < 0.02 AS weibull_sampler_mc;
SELECT abs(provsql.expected(provsql.pareto(1, 4) * provsql.uniform(0, 1))
           - 4.0 / 3 / 2) < 0.02 AS pareto_sampler_mc;
RESET provsql.monte_carlo_seed;

-- (14) Constructor validation.
\set VERBOSITY terse
SELECT provsql.weibull(0, 1);
SELECT provsql.weibull(1, 'Infinity');
SELECT provsql.pareto(-1, 2);
SELECT provsql.pareto(1, 0);
\set VERBOSITY default

-- (15) Both families are in the registry (Studio renders them with no
-- client change).
SELECT name, nparams, param_names, label
  FROM provsql.rv_families() WHERE name IN ('weibull', 'pareto')
 ORDER BY name;

SELECT 'ok'::text AS continuous_weibull_pareto_done;
