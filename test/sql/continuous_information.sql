\set ECHO none
\pset format unaligned

-- Information-theoretic readouts: entropy / kl / mutual_information.
-- The closed-density paths (bare RV quadrature, categorical sums,
-- independent-arm mixture trees) are exact under rv_mc_samples = 0; the
-- estimator paths (composite shapes, conditional entropy, correlated
-- mutual information) use seeded Monte Carlo with loose tolerances.

SET provsql.rv_mc_samples = 0;

-- ---------------------------------------------------------------------
-- Entropy (nats).  Normal: H = ln(2 pi e sigma^2)/2; Uniform(0,4): ln 4;
-- point mass: 0 (Shannon convention); fair coin: ln 2; a well-separated
-- two-component GMM: sum w_i (H_i - ln w_i) = H_component + ln 2.
-- ---------------------------------------------------------------------
SELECT round(entropy(provsql.normal(0, 2))::numeric, 4)  AS h_normal;
SELECT round(entropy(provsql.uniform(0, 4))::numeric, 4) AS h_uniform;
SELECT round(entropy(provsql.as_random(5))::numeric, 4)  AS h_dirac;
SELECT round(entropy(provsql.categorical(ARRAY[0.5, 0.5],
                                         ARRAY[0.0, 1.0]))::numeric, 4)
       AS h_coin;
SELECT abs(entropy(provsql.gmm(ARRAY[0.5, 0.5], ARRAY[0.0, 100.0],
                               ARRAY[1.0, 1.0]))
           - (0.5 * ln(2 * pi() * exp(1.0)) + ln(2.0))) < 0.001
       AS h_gmm_separated;

-- ---------------------------------------------------------------------
-- KL divergence (nats).  Normal-Normal closed form:
--   KL(N(m1,s1) || N(m2,s2)) = ln(s2/s1) + (s1^2+(m1-m2)^2)/(2 s2^2) - 1/2.
-- ---------------------------------------------------------------------
SELECT round(kl(provsql.normal(0, 1), provsql.normal(0, 1))::numeric, 4)
       AS kl_same;
SELECT round(kl(provsql.normal(0, 1), provsql.normal(1, 1))::numeric, 4)
       AS kl_shifted;
SELECT round(kl(provsql.normal(0, 1), provsql.normal(0, 2))::numeric, 4)
       AS kl_widened;
SELECT round(kl(provsql.categorical(ARRAY[0.5, 0.5], ARRAY[0.0, 1.0]),
                provsql.categorical(ARRAY[0.9, 0.1], ARRAY[0.0, 1.0]))::numeric, 4)
       AS kl_coins;
-- P has an outcome Q gives zero mass: not absolutely continuous.
SELECT kl(provsql.categorical(ARRAY[0.5, 0.5], ARRAY[0.0, 1.0]),
          provsql.as_random(0)) AS kl_not_abscont;
-- Mismatched kinds (continuous P vs discrete Q).
SELECT kl(provsql.normal(0, 1),
          provsql.categorical(ARRAY[0.5, 0.5], ARRAY[0.0, 1.0]))
       AS kl_mixed_kinds;
-- Arithmetic composites have no closed density: actionable error.
SELECT kl(provsql.normal(0, 1) + provsql.normal(0, 1), provsql.normal(0, 2));

-- ---------------------------------------------------------------------
-- Mutual information (nats).  Structural independence is an exact 0;
-- a continuous variable with itself diverges.
-- ---------------------------------------------------------------------
SELECT round(mutual_information(provsql.normal(0, 1),
                                provsql.normal(0, 1))::numeric, 4)
       AS mi_independent;
CREATE TABLE mi(x random_variable);
INSERT INTO mi VALUES (provsql.normal(0, 1));
SELECT mutual_information(x, x) AS mi_self_continuous FROM mi;
-- A correlated pair with no MC budget raises.
SELECT mutual_information(x, x + provsql.normal(0, 1)) FROM mi;

-- ---------------------------------------------------------------------
-- Estimator paths (seeded MC).  Bivariate normal Y = X + N(0,1) has
-- rho = 1/sqrt(2), I = -ln(1 - rho^2)/2 = ln(2)/2 ~= 0.3466.  The sum
-- X + N(0,1) is N(0, sqrt(2)): H = ln(2 pi e * 2)/2 ~= 1.7655.
-- Conditional: X | X > 0 is half-normal, H = ln(pi e / 2)/2 ~= 0.7258.
-- ---------------------------------------------------------------------
SET provsql.rv_mc_samples = 100000;
SET provsql.monte_carlo_seed = 11;
SELECT abs(mutual_information(x, x + provsql.normal(0, 1)) - 0.3466) < 0.05
       AS mi_correlated_close FROM mi;
SELECT abs(entropy(x + provsql.normal(0, 1)) - 1.7655) < 0.05
       AS h_composite_close FROM mi;
SELECT abs(entropy(x, rv_cmp_gt(x, as_random(0))) - 0.7258) < 0.05
       AS h_conditional_close FROM mi;
DROP TABLE mi;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS information_done;
