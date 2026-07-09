\set ECHO none
\pset format unaligned

-- Exact conjugate-prior posteriors: when an and_agg evidence conjunction of
-- observe atoms matches a registered conjugate prior/likelihood pair (the
-- latent a bare all-literal leaf wired into ONE parameter slot of every
-- observed leaf), the posterior is computed in closed form -- exact,
-- deterministic, and available with Monte Carlo DISABLED.  Everything below
-- the "decline coverage" marker restores a sample budget and checks that
-- non-conjugate shapes still answer through importance sampling.

SET search_path TO provsql, public;
SET provsql.rv_mc_samples = 0;   -- exact only: any fall-through to sampling raises

-- ---------------------------------------------------------------------
-- Normal-Normal (the case-study-8 Problem 13 workload).  Prior
-- mu ~ N(0, 10); data x_i ~ N(mu, 1); data {8, 10, 12}:
--   posterior precision = 1/100 + 3 = 3.01,
--   mean = 30/3.01 = 9.9667774086...,  variance = 1/3.01 = 0.3322259136...
-- ---------------------------------------------------------------------

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  ev uuid;
BEGIN
  SELECT and_agg(observe(normal(mu, 1), x)) INTO ev
    FROM (VALUES (8.0::float8), (10.0), (12.0)) AS t(x);
  RAISE NOTICE 'normal_normal_mean_exact: %',
    (abs(expected(mu, ev) - 30.0/3.01) < 1e-12);
  RAISE NOTICE 'normal_normal_var_exact: %',
    (abs(variance(mu, ev) - 1.0/3.01) < 1e-12);
  -- Gaussian posterior: the exact median is the mean; the exact 10%
  -- quantile is mean + z_{0.1}·sd (inverse CDF, ~1e-7 grade numerics).
  RAISE NOTICE 'normal_normal_median_exact: %',
    (abs(quantile(mu, 0.5, ev) - 30.0/3.01) < 1e-9);
  RAISE NOTICE 'normal_normal_q10_exact: %',
    (abs(quantile(mu, 0.1, ev) - (30.0/3.01 - 1.2815515655446004 * sqrt(1.0/3.01))) < 1e-4);
  -- Marginal likelihood P(data): the exact N(0, I + 100·11^T) density at
  -- (8,10,12), i.e. (2*pi)^{-3/2} · 301^{-1/2} · exp(-(308 - 90000/301)/2).
  RAISE NOTICE 'normal_normal_evidence_exact: %',
    (abs(evidence(ev)
         - exp(-(308.0 - 90000.0/301.0)/2.0) / (power(2*pi(), 1.5) * sqrt(301.0))) < 1e-18);
  -- Differential entropy of the Gaussian posterior: ln(2*pi*e*var)/2 nats
  -- (exact quadrature over the family pdf).
  RAISE NOTICE 'normal_normal_entropy_exact: %',
    (abs(entropy(mu, ev) - 0.5 * ln(2*pi()*exp(1.0)/3.01)) < 1e-3);
END $$;

-- ---------------------------------------------------------------------
-- Exact posterior sampling and histogram, still at rv_mc_samples = 0:
-- rv_sample draws i.i.d. from the posterior distribution (seeded), and
-- rv_histogram bins the exact analytical shape (masses sum to ~1).
-- ---------------------------------------------------------------------

SET provsql.monte_carlo_seed = 42;
DO $$
DECLARE
  mu random_variable := normal(0, 10);
  ev uuid;
  n bigint;
  m double precision;
  tot double precision;
BEGIN
  SELECT and_agg(observe(normal(mu, 1), x)) INTO ev
    FROM (VALUES (8.0::float8), (10.0), (12.0)) AS t(x);
  SELECT count(*), avg(s) INTO n, m FROM rv_sample((mu)::uuid, 20000, ev) s;
  RAISE NOTICE 'posterior_sample_count: %', n;
  RAISE NOTICE 'posterior_sample_mean_ok: %', (abs(m - 30.0/3.01) < 0.02);
  SELECT sum((b->>'count')::float8) INTO tot
    FROM jsonb_array_elements(rv_histogram((mu)::uuid, 25, ev)) b;
  RAISE NOTICE 'posterior_histogram_mass_ok: %', (abs(tot - 1.0) < 1e-3);
END $$;

-- ---------------------------------------------------------------------
-- Gamma-prior rate, three likelihood routes into the same carrier:
-- Exponential gaps, Poisson counts, and their MIXED interleaving (the
-- update is per observation against the running posterior, so mixed
-- likelihoods sharing one conjugate prior compose).  gamma(2, 1) with an
-- integer shape is stored as an erlang leaf; the fold canonicalises it
-- into the gamma carrier.
-- ---------------------------------------------------------------------

DO $$
DECLARE
  lam random_variable := gamma(2, 1);
  ev uuid;
BEGIN
  -- Exponential data {0.5, 1.0, 1.5}: Gamma(2+3, 1+3) = Gamma(5, 4).
  SELECT and_agg(observe(exponential(lam), x)) INTO ev
    FROM (VALUES (0.5::float8), (1.0), (1.5)) AS t(x);
  RAISE NOTICE 'gamma_exp_mean: %', expected(lam, ev);
  RAISE NOTICE 'gamma_exp_var: %', variance(lam, ev);
  -- Sequential Lomax predictives:
  --   2·1²/1.5³ · 3·1.5³/2.5⁴ · 4·2.5⁴/4⁵ = 24/1024 = 0.0234375.
  RAISE NOTICE 'gamma_exp_evidence_exact: %',
    (abs(evidence(ev) - 24.0/1024.0) < 1e-15);

  -- Poisson counts {3, 5}: Gamma(2+8, 1+2) = Gamma(10, 3), mean 10/3.
  SELECT and_agg(observe(poisson(lam), x)) INTO ev
    FROM (VALUES (3.0::float8), (5.0)) AS t(x);
  RAISE NOTICE 'gamma_poisson_mean_exact: %',
    (abs(expected(lam, ev) - 10.0/3.0) < 1e-12);
  -- Negative-binomial predictive factors, exact:
  --   m(3) = C(4,3)·(1/2)^5 ... folded: Gamma(2,1) -> Gamma(5,2) -> Gamma(10,3).
  RAISE NOTICE 'gamma_poisson_evidence_exact: %',
    (abs(evidence(ev)
         - (4.0*3*2/6) * power(0.5, 5)
           * (9.0*8*7*6/24) * power(2.0/3, 5) * power(1.0/3, 5)) < 1e-15);

  -- Mixed fold: one Poisson count (4) and one Exponential gap (2):
  -- Gamma(2+4+1, 1+1+2) = Gamma(7, 4), mean 7/4.
  SELECT provenance_times(observe(poisson(lam), 4),
                          observe(exponential(lam), 2.0)) INTO ev;
  RAISE NOTICE 'gamma_mixed_mean: %', expected(lam, ev);
END $$;

-- ---------------------------------------------------------------------
-- Beta-prior success probability: Binomial, Geometric (trials
-- convention), NegativeBinomial (failures convention).
-- ---------------------------------------------------------------------

DO $$
DECLARE
  p random_variable := beta(2, 3);
  ev uuid;
BEGIN
  -- Binomial(10, p) = 7: Beta(2+7, 3+3) = Beta(9, 6), mean 9/15 = 0.6.
  ev := observe(binomial(10, p), 7);
  RAISE NOTICE 'beta_binomial_mean: %', expected(p, ev);
  -- Beta-binomial predictive C(10,7)·B(9,6)/B(2,3), exact via lgamma.
  RAISE NOTICE 'beta_binomial_evidence_exact: %',
    (abs(evidence(ev)
         - 120.0 * exp(lgamma(9.0) + lgamma(6.0) - lgamma(15.0)
                       - (lgamma(2.0) + lgamma(3.0) - lgamma(5.0)))) < 1e-15);

  -- Geometric (trials on {1, 2, ...}), d = 4: Beta(2+1, 3+3) = Beta(3, 6).
  ev := observe(geometric(p), 4);
  RAISE NOTICE 'beta_geometric_mean_exact: %',
    (abs(expected(p, ev) - 1.0/3.0) < 1e-12);

  -- NegativeBinomial(r = 3, p) (failures on {0, 1, ...}), d = 5:
  -- Beta(2+3, 3+5) = Beta(5, 8), mean 5/13.
  ev := observe(negative_binomial(3, p), 5);
  RAISE NOTICE 'beta_negbinomial_mean_exact: %',
    (abs(expected(p, ev) - 5.0/13.0) < 1e-12);
END $$;

-- ---------------------------------------------------------------------
-- LogNormal-Normal (log-scale location), Pareto-Uniform (upper bound of
-- U(0, theta)), Gamma-Pareto (tail shape).
-- ---------------------------------------------------------------------

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  theta random_variable := pareto(1, 2);
  alpha random_variable := gamma(3, 2);
  ev uuid;
BEGIN
  -- LogNormal(mu, 1) observed at e^2: a Normal update at ln d = 2, so the
  -- posterior mean is 2·100/101.
  ev := observe(lognormal(mu, 1), exp(2.0));
  RAISE NOTICE 'lognormal_normal_mean_exact: %',
    (abs(expected(mu, ev) - 200.0/101.0) < 1e-12);

  -- U(0, theta) observed at {0.5, 3}: Pareto(1, 2) -> Pareto(3, 4), mean
  -- 3·4/3 = 4.  (A nonzero literal lower bound is NOT conjugate and
  -- declines -- see the decline section.)
  SELECT and_agg(observe(uniform(0, theta), x)) INTO ev
    FROM (VALUES (0.5::float8), (3.0)) AS t(x);
  RAISE NOTICE 'pareto_uniform_mean: %', expected(theta, ev);
  -- Predictives: d=0.5 <= xm: alpha/((alpha+1)·xm) = 2/3;
  -- then Pareto(1,3) at d=3 > xm: 3·1³/(4·3⁴) = 3/324.
  RAISE NOTICE 'pareto_uniform_evidence_exact: %',
    (abs(evidence(ev) - (2.0/3.0) * (3.0/324.0)) < 1e-15);

  -- Pareto(1, alpha) observed at e: Gamma(3, 2) -> Gamma(4, 3), mean 4/3.
  ev := observe(pareto(1, alpha), exp(1.0));
  RAISE NOTICE 'gamma_pareto_mean_exact: %',
    (abs(expected(alpha, ev) - 4.0/3.0) < 1e-12);
END $$;

-- ---------------------------------------------------------------------
-- The conditional-equality surface routes here too: "R | (Y = d)" and
-- given(Y = d) rewrite the point event into an observe atom, so a bare
-- conjugate latent stays exact at rv_mc_samples = 0.
-- ---------------------------------------------------------------------

DO $$
DECLARE
  lam random_variable := gamma(2, 1);
BEGIN
  -- lam | (poisson(lam) = 3): Gamma(5, 2), mean 2.5.
  RAISE NOTICE 'equality_form_mean: %', expected(lam | (poisson(lam) = 3));
END $$;

-- ---------------------------------------------------------------------
-- Decline coverage: shapes one step outside the recogniser fall back to
-- importance sampling (restored sample budget) and stay CORRECT -- the
-- closed form computes the same estimand the sampler estimates.
-- ---------------------------------------------------------------------

SET provsql.rv_mc_samples = 300000;
SET provsql.monte_carlo_seed = 20260709;
SET provsql.ess_warn_fraction = 0;

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  sig random_variable := gamma(2, 2);
  a random_variable := normal(0, 10);
  b random_variable := normal(0, 10);
  theta2 random_variable := pareto(2, 3);
  ev uuid;
BEGIN
  -- (a) latent reaching the leaf through gate_arith: IS, within tolerance
  -- of the affine-transformed closed form (posterior of mu given
  -- 2·mu + noise = 8 has mean 8·2·100/(4·100+1) = 1600/401 = 3.9900...).
  ev := observe(normal(mu + mu, 1), 8);
  RAISE NOTICE 'decline_arith_is_ok: %',
    (abs(expected(mu, ev) - 1600.0/401.0) < 0.05);

  -- (b) wired sigma slot: no registered rule (conjugacy is on the
  -- precision, not sigma); IS still answers.
  ev := observe(normal(0, sig), 1.5);
  RAISE NOTICE 'decline_sigma_slot_is_ok: %',
    (expected(sig, ev) > 0.5 AND expected(sig, ev) < 2.0);

  -- (c) a Boolean factor conjoined with the observations declines the
  -- recognition but not the semantics: an independent Bernoulli factor
  -- leaves the posterior unchanged.
  SELECT provenance_times(and_agg(observe(normal(mu, 1), x)),
                          rv_cmp_lt(uniform(0, 1), as_random(0.5))) INTO ev
    FROM (VALUES (8.0::float8), (10.0), (12.0)) AS t(x);
  RAISE NOTICE 'decline_boolean_factor_is_ok: %',
    (abs(expected(mu, ev) - 30.0/3.01) < 0.05);

  -- (d) two distinct latents coupled by one evidence set: joint posterior,
  -- out of the one-spec carrier; IS answers (a's posterior mean given one
  -- observation of a at 8 is 800/101, unaffected by b's observation).
  SELECT provenance_times(observe(normal(a, 1), 8),
                          observe(normal(b, 1), 9)) INTO ev;
  RAISE NOTICE 'decline_two_latents_is_ok: %',
    (abs(expected(a, ev) - 800.0/101.0) < 0.1);

  -- (e) nonzero literal lower bound of a uniform: not Pareto-conjugate
  -- (the 1/(theta - 1) kernel is not Pareto-shaped in theta itself); IS
  -- answers.  The Pareto(2, 3) prior keeps theta >= 2 > 1, so the leaf's
  -- parameters stay in-domain on every prior draw.
  ev := observe(uniform(1, theta2), 1.5);
  RAISE NOTICE 'decline_nonzero_uniform_lower_is_ok: %',
    (expected(theta2, ev) > 2.0 AND expected(theta2, ev) < 3.0);
END $$;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;
RESET provsql.ess_warn_fraction;
