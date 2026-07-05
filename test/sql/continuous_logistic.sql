\set ECHO none
\pset format unaligned

-- Logistic(μ, s): the location-scale family whose CDF is the sigmoid and
-- whose quantile is the logit.  All the readouts below are closed-form exact
-- (rv_mc_samples = 0), including the logit link P(eps < c) = sigma(c) that is
-- the whole reason to prefer Logistic over Normal for a log-odds selection.

SET search_path TO provsql, public;
SET provsql.rv_mc_samples = 0;

-- Moments: mean = μ, variance = π² s² / 3 (both exact / affine in the params).
SELECT abs(expected(logistic(2, 0.5)) - 2.0) < 1e-9              AS mean_is_mu,
       abs(variance(logistic(2, 0.5)) - pi() * pi() * 0.25 / 3.0) < 1e-9
                                                                 AS var_is_pi2s2_3;

-- Raw second moment of Logistic(0,1) is the variance π²/3 (mean 0).
SELECT abs(moment(logistic(0, 1), 2) - pi() * pi() / 3.0) < 1e-9 AS raw_moment2_exact;

-- Quantile is the logit: Q(σ(2)) = 2, Q(0.5) = μ.
SELECT abs(quantile(logistic(0, 1), 1.0 / (1.0 + exp(-2.0))) - 2.0) < 1e-6 AS quantile_is_logit,
       abs(quantile(logistic(3, 2), 0.5) - 3.0) < 1e-9                     AS median_is_mu;

-- The logit LINK, exact: P(eps < c) = σ(c) for eps ~ Logistic(0,1).  This is
-- the analytic CDF the AnalyticEvaluator resolves on the comparator gate.
CREATE TABLE lg AS SELECT logistic(0, 1) AS eps;
SELECT add_provenance('lg');
DO $$
DECLARE e2 uuid; en uuid;
BEGIN
  SELECT provenance() INTO e2 FROM lg WHERE eps < 2.0;
  SELECT provenance() INTO en FROM lg WHERE eps < -1.0;
  RAISE NOTICE 'link_sigma_2: %', (abs(probability(e2) - 1.0 / (1.0 + exp(-2.0))) < 1e-6);
  RAISE NOTICE 'link_sigma_minus1: %', (abs(probability(en) - 1.0 / (1.0 + exp(1.0))) < 1e-6);
END $$;
DROP TABLE lg;

-- Affine closure a·X + b stays Logistic(aμ+b, |a|s): mean and variance transform
-- exactly (mean affine, so no MC even for the compound arithmetic).
SELECT abs(expected(logistic(0, 1) * 2.0 + 1.0) - 1.0) < 1e-9                   AS affine_mean_exact,
       abs(variance(logistic(0, 1) * 2.0 + 1.0) - 4.0 * pi() * pi() / 3.0) < 1e-9 AS affine_var_exact;

-- s = 0 degenerates to the Dirac at μ (routed through as_random, like normal).
SELECT abs(expected(logistic(5, 0)) - 5.0) < 1e-9  AS degenerate_is_dirac,
       abs(variance(logistic(5, 0)))       < 1e-9  AS degenerate_var_zero;

RESET provsql.rv_mc_samples;
