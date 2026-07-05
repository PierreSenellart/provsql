\set ECHO none
\pset format unaligned

-- Latent variables through an AGGREGATE (use case: sensor-field estimation).
-- A shared latent that every row references does NOT average away, so an
-- aggregate over the rows is correlation-aware: its variance is floored at
-- the latent's variance rather than shrinking like 1/n.  A data-derived prior
-- (the aggregate) is then updated by conditioning a new, same-latent reading
-- on an interval.  Everything is jointly Gaussian; the readouts are seeded-MC
-- tolerance checks.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 424242;
SET provsql.rv_mc_samples = 300000;

-- Five sensors with known true values; readings share a calibration drift
-- d ~ Normal(0, 1):  reading_i = Normal(true_i + d, 0.3).
CREATE TEMP TABLE truth(id int, mu float);
INSERT INTO truth VALUES (1, 20.1), (2, 19.8), (3, 20.4), (4, 20.0), (5, 19.9);
-- mean of the true values = 20.04.

CREATE TEMP TABLE drift(d random_variable);
INSERT INTO drift VALUES (normal(0, 1.0));   -- tau_d = 1, materialised ONCE (shared)

CREATE TEMP TABLE reading AS
  SELECT t.id, normal(mu + d, 0.3) AS y FROM truth t, drift;

-- Data-derived prior = AVG of the readings.  The shared drift does not average
-- away, so Var(mu_prior) ~ tau_d^2 + 0.3^2/5 = 1.018, NOT 0.3^2/5 = 0.018.
CREATE TEMP TABLE prior AS SELECT avg(y) AS mu_prior FROM reading;

DO $$
DECLARE e double precision; v double precision;
BEGIN
  SELECT expected(mu_prior), variance(mu_prior) INTO e, v FROM prior;
  RAISE NOTICE 'prior_mean_is_true_average: %', (abs(e - 20.04) < 0.05);
  -- The correlation-aware point: the variance is floored near tau_d^2 = 1,
  -- an order of magnitude above the independence value 0.018.
  RAISE NOTICE 'prior_var_floored_by_shared_latent: %', (abs(v - 1.018) < 0.15);
END $$;

-- Contrast: give every reading its OWN independent drift.  Now the drift DOES
-- average away, so Var(mu_prior_ind) ~ 1/5 + 0.3^2/5 = 0.218 -- far below the
-- shared value.  This is what makes the shared-latent case correlation-aware.
CREATE TEMP TABLE reading_ind AS
  SELECT t.id, normal(mu + normal(0, 1.0), 0.3) AS y FROM truth t;
CREATE TEMP TABLE prior_ind AS SELECT avg(y) AS mu_prior FROM reading_ind;

DO $$
DECLARE v_shared double precision; v_ind double precision;
BEGIN
  SELECT variance(mu_prior) INTO v_shared FROM prior;
  SELECT variance(mu_prior) INTO v_ind    FROM prior_ind;
  RAISE NOTICE 'independent_drift_averages_away: %', (v_ind < 0.35);
  RAISE NOTICE 'shared_variance_exceeds_independent: %', (v_shared > 2.0 * v_ind);
END $$;

-- Bayesian update: a new reading off the SAME batch (shares both mu_prior and
-- the drift d); condition the region's true value on that reading landing in
-- [21.0, 21.2].  Jointly Gaussian, so this is a Kalman-type update; the shared
-- drift carries the correlation into the posterior, pulling mu_prior above its
-- prior mean of 20.04.
CREATE TEMP TABLE newobs AS
  SELECT normal(mu_prior + d, 0.3) AS y_new, mu_prior FROM prior, drift;

DO $$
DECLARE post double precision;
BEGIN
  SELECT expected(mu_prior | (y_new > 21.0 AND y_new < 21.2)) INTO post FROM newobs;
  RAISE NOTICE 'posterior_pulled_above_prior: %', (post > 20.2 AND post < 21.2);
END $$;
