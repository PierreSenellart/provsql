\set ECHO none
\pset format unaligned

-- Latent variables, Part B: likelihood-weighting posterior inference.
-- observe(X, d) binds a datum to a latent-dependent gate_rv leaf; and_agg
-- conjoins the per-observation evidence; the moment / quantile / sample
-- readouts, passed that evidence as their conditioning argument, report the
-- posterior via self-normalised importance sampling.  Seeded-MC tolerance
-- checks against closed-form conjugate posteriors.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 20260705;
SET provsql.rv_mc_samples = 200000;
SET provsql.ess_warn_fraction = 0;   -- silence ESS warnings except where tested

-- ---------------------------------------------------------------------
-- Normal-Normal conjugacy.  Prior mu ~ N(0, 10); data x_i ~ N(mu, 1);
-- data {8, 10, 12} (n = 3, sum = 30).  Posterior:
--   1/tau_n^2 = 1/100 + 3/1 = 3.01  ->  var = 0.33223, mean = 9.9668.
-- ---------------------------------------------------------------------

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  ev uuid;
  m double precision;
  v double precision;
BEGIN
  SELECT and_agg(observe(normal(mu, 1), x)) INTO ev
    FROM (VALUES (8.0::float8), (10.0), (12.0)) AS t(x);
  m := expected(mu, ev);
  v := variance(mu, ev);
  RAISE NOTICE 'normal_normal_mean: %', (abs(m - 9.9668) < 0.1);
  RAISE NOTICE 'normal_normal_var: %',  (abs(v - 0.33223) < 0.05);
  -- Posterior median ~ posterior mean for a (symmetric) Gaussian posterior.
  RAISE NOTICE 'normal_normal_median: %', (abs(quantile(mu, 0.5, ev) - 9.9668) < 0.15);
  -- Marginal likelihood P(data): closed form is the N(0, I + 100*11^T)
  -- density at (8,10,12) = exp(-10.11203) = 4.058e-5.
  RAISE NOTICE 'normal_normal_evidence: %', (abs(evidence(ev) / 4.058e-5 - 1.0) < 0.05);
END $$;

-- ---------------------------------------------------------------------
-- Gamma-Exponential conjugacy.  Prior rate lambda ~ Gamma(shape 2, rate 1);
-- data x_i ~ Exponential(lambda); data {0.5, 1.0, 1.5} (n = 3, sum = 3).
-- Posterior lambda ~ Gamma(2 + 3, 1 + 3) = Gamma(5, 4):
--   mean = 5/4 = 1.25, var = 5/16 = 0.3125.
-- ---------------------------------------------------------------------

DO $$
DECLARE
  lambda random_variable := gamma(2, 1);
  ev uuid;
BEGIN
  SELECT and_agg(observe(exponential(lambda), x)) INTO ev
    FROM (VALUES (0.5::float8), (1.0), (1.5)) AS t(x);
  RAISE NOTICE 'gamma_exp_mean: %', (abs(expected(lambda, ev) - 1.25) < 0.06);
  RAISE NOTICE 'gamma_exp_var: %',  (abs(variance(lambda, ev) - 0.3125) < 0.06);
END $$;

-- ---------------------------------------------------------------------
-- Posterior predictive: rv_sample on a fresh leaf reusing the latent draws
-- from the same evidence.  The predictive mean tracks the posterior mean.
-- ---------------------------------------------------------------------

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  ev uuid;
  pred_mean double precision;
BEGIN
  SELECT and_agg(observe(normal(mu, 1), x)) INTO ev
    FROM (VALUES (8.0::float8), (10.0), (12.0)) AS t(x);
  SELECT avg(s) INTO pred_mean FROM rv_sample((normal(mu, 1))::uuid, 20000, ev) s;
  RAISE NOTICE 'posterior_predictive_mean: %', (abs(pred_mean - 9.9668) < 0.2);
END $$;

-- ---------------------------------------------------------------------
-- Shapley attribution: with three tight observations and one outlier, the
-- outlier gets the largest-magnitude posterior-mean attribution, and the
-- values sum to the prior->posterior shift (Shapley efficiency).
-- ---------------------------------------------------------------------

DO $$
DECLARE
  mu random_variable := normal(0, 10);
  toks uuid[];
  ev uuid;
  total double precision;
  outlier_shap double precision;
  max_other double precision;
  post_shift double precision;
BEGIN
  -- Build the evidence with and_agg (a left-nested gate_times), and keep the
  -- atoms ordered by x so toks[4] is the outlier; shapley_observe recovers the
  -- flat atom set through the recursive observe_atoms collector.
  SELECT array_agg(o ORDER BY x), and_agg(o)
    INTO toks, ev
    FROM (SELECT x, observe(normal(mu, 1), x) AS o
          FROM (VALUES (5.0::float8), (5.5), (4.5), (30.0)) AS t(x)) s;
  -- toks[4] is the outlier (x = 30, largest under ORDER BY x).  Compute the
  -- attribution once and reuse it.
  CREATE TEMP TABLE sh_result ON COMMIT DROP AS
    SELECT observation, value FROM shapley_observe((mu)::uuid, ev, 'expected');
  SELECT value INTO outlier_shap FROM sh_result WHERE observation = toks[4];
  SELECT max(abs(value)) INTO max_other FROM sh_result WHERE observation <> toks[4];
  SELECT sum(value) INTO total FROM sh_result;
  post_shift := expected(mu, ev) - expected(mu, gate_one());
  RAISE NOTICE 'shapley_outlier_dominates: %', (abs(outlier_shap) > max_other);
  RAISE NOTICE 'shapley_efficiency: %', (abs(total - post_shift) < 0.15);
END $$;

-- ---------------------------------------------------------------------
-- ESS diagnostic: many tight observations over a broad prior degenerate the
-- weights; with the default warn fraction a WARNING is emitted (its counts
-- depend on the RNG stream, so it is suppressed here to keep the expected
-- output platform-independent -- the low-ESS path is still exercised, and
-- the posterior stays correct).
-- ---------------------------------------------------------------------

SET provsql.ess_warn_fraction = 0.5;
SET provsql.rv_mc_samples = 100000;
DO $$
DECLARE
  mu random_variable := normal(0, 50);
  ev uuid;
  m double precision;
BEGIN
  SELECT and_agg(observe(normal(mu, 1), x)) INTO ev
    FROM (VALUES (20.0::float8), (20.1), (19.9), (20.05), (19.95), (20.0)) AS t(x);
  PERFORM set_config('client_min_messages', 'error', true);   -- hide the WARNING
  m := expected(mu, ev);   -- degenerate ESS: emits (suppressed) low-ESS WARNING
  PERFORM set_config('client_min_messages', 'notice', true);
  RAISE NOTICE 'ess_posterior_mean_ok: %', (abs(m - 20.0) < 0.5);
END $$;
