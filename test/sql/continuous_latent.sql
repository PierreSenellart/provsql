\set ECHO none
\pset format unaligned

-- Latent variables, Part A: a distribution parameter may itself be a
-- random variable (a compound / hierarchical distribution).  The leaf is
-- sampled per Monte Carlo iteration; every analytic path recognises the
-- wired form and falls through to MC, so these are seeded-MC tolerance
-- checks rather than exact closed forms.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 300000;

-- ---------------------------------------------------------------------
-- Compound moments.  For M ~ Normal(0, s):
--   E[Normal(M, 1)]   = E[M]          = 0
--   Var[Normal(M, 1)] = 1 + Var(M)    = 1 + s^2
-- ---------------------------------------------------------------------

-- E[Normal(Normal(0,3), 1)] = 0.
SELECT abs(expected(normal(normal(0, 3), 1))) < 0.1 AS compound_mean_zero;

-- Var[Normal(Normal(0,3), 1)] = 1 + 9 = 10.
SELECT abs(variance(normal(normal(0, 3), 1)) - 10.0) < 0.6 AS compound_var_ten;

-- A latent rate: E[Exponential(L)] with L ~ Uniform(1, 3).  Given L,
-- E[Exp(L)] = 1/L, so the compound mean is E[1/L] = (ln 3 - ln 1)/2
-- = 0.549306.
SELECT abs(expected(exponential(uniform(1.0, 3.0))) - 0.549306) < 0.02
       AS compound_exp_rate;

-- A latent scale on a Gamma: shape=2 fixed, rate ~ Uniform(1, 2).
-- E[Gamma(2, R)] = 2/R, compound = 2*E[1/R] = 2*(ln 2)/1 = 1.386294.
SELECT abs(expected(gamma(2.0, uniform(1.0, 2.0))) - 1.386294) < 0.03
       AS compound_gamma_rate;

-- ---------------------------------------------------------------------
-- Shared-latent coupling.  Two leaves Normal(M, 1) over the SAME M
-- couple through the sampler's per-iteration scalar cache, so
-- Cov(X, Y) = E[XY] - E[X]E[Y] = E[M^2] = Var(M).
-- ---------------------------------------------------------------------

DO $$
DECLARE
  m random_variable := normal(0, 2);   -- Var(M) = 4
  x random_variable;
  y random_variable;
  cov double precision;
BEGIN
  x := normal(m, 1);
  y := normal(m, 1);
  cov := expected(x * y) - expected(x) * expected(y);
  IF abs(cov - 4.0) < 0.3 THEN
    RAISE NOTICE 'shared_latent_coupling: t';
  ELSE
    RAISE NOTICE 'shared_latent_coupling: f (cov=%)', cov;
  END IF;
END $$;

-- Independent latents (two DISTINCT M draws) are uncorrelated.
DO $$
DECLARE
  x random_variable := normal(normal(0, 2), 1);
  y random_variable := normal(normal(0, 2), 1);
  cov double precision;
BEGIN
  cov := expected(x * y) - expected(x) * expected(y);
  IF abs(cov) < 0.3 THEN
    RAISE NOTICE 'independent_latent_uncorrelated: t';
  ELSE
    RAISE NOTICE 'independent_latent_uncorrelated: f (cov=%)', cov;
  END IF;
END $$;

-- ---------------------------------------------------------------------
-- Parameter-domain guard: a prior that can emit a scale <= 0 raises a
-- specific, actionable error rather than silently truncating the prior.
-- ---------------------------------------------------------------------

DO $$
DECLARE r double precision;
BEGIN
  r := expected(normal(0, normal(0, 3)));   -- sigma latent draws <= 0
  RAISE NOTICE 'domain_guard: f (returned %)', r;
EXCEPTION WHEN OTHERS THEN
  IF SQLERRM LIKE '%outside the family''s domain%' THEN
    RAISE NOTICE 'domain_guard: t';
  ELSE
    RAISE NOTICE 'domain_guard: f (wrong error: %)', SQLERRM;
  END IF;
END $$;

-- ---------------------------------------------------------------------
-- Back-compatibility: the all-literal call resolves to the plain numeric
-- constructor (exact closed form), unaffected by the token overloads.
-- ---------------------------------------------------------------------

SET provsql.rv_mc_samples = 0;   -- forbid MC: literal path must be analytic
SELECT expected(normal(2.5, 0.5)) = 2.5 AS literal_mean_exact;
SELECT variance(normal(2.5, 0.5)) = 0.25 AS literal_var_exact;
SELECT expected(gamma(2.0, 4.0)) = 0.5 AS literal_gamma_mean_exact;
