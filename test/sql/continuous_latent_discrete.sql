\set ECHO none
\pset format unaligned

-- Latent variables, discrete families: poisson(random_variable) and
-- binomial(integer, random_variable) are parametric gate_rv leaves (the
-- literal-parameter constructors still enumerate an exact categorical).
-- They reuse the whole latent machinery: per-draw sampling, the Poisson /
-- Binomial pmf as the observe likelihood weight, and the affine-mean fast
-- path (Poisson mean = lambda).  Discrete-likelihood posteriors (Gamma-
-- Poisson, Beta-Binomial) are conjugate, so the IS posterior matches the
-- closed form.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 20260706;
SET provsql.rv_mc_samples = 400000;
SET provsql.ess_warn_fraction = 0;

-- ---------------------------------------------------------------------
-- Forward compound moments.
--   E[Poisson(Lambda)]   = E[Lambda]              (affine mean, exact)
--   Var[Poisson(Lambda)] = E[Lambda] + Var[Lambda] (law of total variance)
--   E[Binomial(n, P)]    = n E[P]
-- ---------------------------------------------------------------------

-- E[Poisson(Gamma(2,1))] = E[Gamma(2,1)] = 2, EXACT (no MC).
SET provsql.rv_mc_samples = 0;
SELECT expected(poisson(gamma(2, 1))) = 2.0 AS poisson_mean_exact;
SET provsql.rv_mc_samples = 400000;

-- Var[Poisson(Uniform(4,6))] = 5 + (6-4)^2/12 = 5.3333.
SELECT abs(variance(poisson(uniform(4, 6))) - 5.3333) < 0.15 AS poisson_var_total;

-- E[Binomial(50, Uniform(0,1))] = 50 * 0.5 = 25.
SELECT abs(expected(binomial(50, uniform(0, 1))) - 25.0) < 0.3 AS binomial_mean;

-- ---------------------------------------------------------------------
-- Gamma-Poisson conjugacy.  Prior R ~ Gamma(shape 2, rate 1); counts
-- x_i ~ Poisson(R); data {3, 5, 4} (n = 3, sum = 12).  Posterior
-- R ~ Gamma(2 + 12, 1 + 3) = Gamma(14, 4): mean = 3.5, var = 14/16 = 0.875.
-- ---------------------------------------------------------------------

DO $$
DECLARE r random_variable := gamma(2, 1); ev uuid;
BEGIN
  SELECT and_agg(observe(poisson(r), x)) INTO ev
    FROM (VALUES (3.0::float8), (5.0), (4.0)) AS t(x);
  RAISE NOTICE 'gamma_poisson_mean: %', (abs(expected(r, ev) - 3.5) < 0.1);
  RAISE NOTICE 'gamma_poisson_var: %',  (abs(variance(r, ev) - 0.875) < 0.1);
END $$;

-- ---------------------------------------------------------------------
-- Beta-Binomial conjugacy.  Prior p ~ Beta(2, 2); successes k_i ~
-- Binomial(10, p); data {7, 8, 6} (sum k = 21, sum failures = 9).  Posterior
-- p ~ Beta(2 + 21, 2 + 9) = Beta(23, 11): mean = 23/34 = 0.6765.
-- ---------------------------------------------------------------------

DO $$
DECLARE p random_variable := beta(2, 2); ev uuid;
BEGIN
  SELECT and_agg(observe(binomial(10, p), k)) INTO ev
    FROM (VALUES (7.0::float8), (8.0), (6.0)) AS t(k);
  RAISE NOTICE 'beta_binomial_mean: %', (abs(expected(p, ev) - 0.6765) < 0.02);
END $$;

-- ---------------------------------------------------------------------
-- Parameter-domain guard: a rate/probability prior that leaves the family's
-- domain raises rather than silently truncating.
-- ---------------------------------------------------------------------

-- Poisson rate that can draw <= 0 (variance samples the rate).
DO $$
BEGIN
  PERFORM variance(poisson(normal(0, 3)));
  RAISE NOTICE 'poisson_rate_guard: f';
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'poisson_rate_guard: %', (SQLERRM LIKE '%outside the family''s domain%');
END $$;

-- Binomial probability that can leave [0, 1].
DO $$
BEGIN
  PERFORM expected(binomial(10, normal(0.5, 1)));
  RAISE NOTICE 'binomial_prob_guard: f';
EXCEPTION WHEN OTHERS THEN
  RAISE NOTICE 'binomial_prob_guard: %', (SQLERRM LIKE '%outside the family''s domain%');
END $$;
