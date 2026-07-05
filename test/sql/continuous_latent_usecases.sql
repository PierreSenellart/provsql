\set ECHO none
\pset format unaligned

-- Latent generative models over correlated aggregates (see usecases.sql):
-- a logit-link selection whose per-tuple probabilities share a latent gate
-- yields an over-dispersed COUNT, and a discrete latent is then conditioned on
-- that count (rv = agg_token), a discrete conjugate posterior.  uc2 (the
-- linear-Gaussian sensor field) is covered by continuous_latent_aggregate.
-- Small models + seeded-MC tolerance checks (the exact link CDF is tested in
-- continuous_logistic); the point here is the correlated aggregate and that
-- the rv = agg_token conditioning runs and lands a sane posterior.

SET search_path TO provsql, public;
SET provsql.monte_carlo_seed = 1;
SET provsql.rv_mc_samples = 20000;

-- ---------------------------------------------------------------------------
-- uc1  Epidemic nowcasting: count of logit-link diagnoses sharing a classifier
--      bias, then a Gamma-Poisson posterior for a rate conditioned on the count.
-- ---------------------------------------------------------------------------
CREATE TABLE patient(id int, alpha float);
INSERT INTO patient SELECT g, ((g % 7) - 3) FROM generate_series(1, 40) g;

CREATE TABLE bias(b random_variable);
INSERT INTO bias VALUES (normal(0, 0.7));                 -- ONE shared bias

CREATE TABLE diagnosis AS
  SELECT p.id, p.alpha, logistic(0, 1) AS eps, b FROM patient p, bias;
-- contrast: every patient gets its OWN bias (the coupling removed)
CREATE TABLE diagnosis_ind AS
  SELECT p.id, p.alpha, logistic(0, 1) AS eps, normal(0, 0.7) AS b FROM patient p;
SELECT add_provenance('diagnosis');
SELECT add_provenance('diagnosis_ind');

CREATE TABLE nowcast     AS SELECT count(*) AS c FROM diagnosis     WHERE eps < alpha + b;
CREATE TABLE nowcast_ind AS SELECT count(*) AS c FROM diagnosis_ind WHERE eps < alpha + b;

DO $$
DECLARE es double precision; vs double precision;
        ei double precision; vi double precision; post_r double precision;
BEGIN
  SELECT expected(c), variance(c) INTO es, vs FROM nowcast;
  SELECT expected(c), variance(c) INTO ei, vi FROM nowcast_ind;
  -- The shared classifier bias couples the diagnoses: the count is
  -- over-dispersed relative to the independent-bias Poisson-binomial, while
  -- the mean (linear) is unchanged.
  RAISE NOTICE 'uc1_count_means_match: %', (abs(es - ei) < 2.5);
  RAISE NOTICE 'uc1_shared_bias_overdisperses: %', (vs > 1.3 * vi);
  -- Gamma-Poisson posterior: R prior mean 1, model count ~ Poisson(20 R), so
  -- the posterior is pulled toward C/20 (~1); conditioning on rv = agg_token.
  SELECT expected(r | (poisson(20 * r) = c)) INTO post_r
    FROM (SELECT gamma(2, 2) AS r) g, nowcast;
  RAISE NOTICE 'uc1_gamma_poisson_posterior_in_range: %', (post_r > 0.6 AND post_r < 1.4);
END $$;

-- ---------------------------------------------------------------------------
-- uc3  Capture-recapture: uncertain recapture count sharing a detectability
--      gate, then a Beta-Binomial posterior for the population size N.
-- ---------------------------------------------------------------------------
CREATE TABLE recapture(j int, logit_match float);
INSERT INTO recapture SELECT g, ((g % 5) - 2) * 0.5 FROM generate_series(1, 30) g;

CREATE TABLE fieldcond(s random_variable);
INSERT INTO fieldcond VALUES (normal(0, 0.5));            -- shared detectability

CREATE TABLE matched AS
  SELECT r.j, r.logit_match, logistic(0, 1) AS eps, s FROM recapture r, fieldcond;
SELECT add_provenance('matched');

CREATE TABLE recap_count AS
  SELECT count(*) AS m FROM matched WHERE eps < logit_match + s;

DO $$
DECLARE em double precision; post_n double precision;
BEGIN
  SELECT expected(m) INTO em FROM recap_count;
  RAISE NOTICE 'uc3_recapture_mean_near_half: %', (em > 10.0 AND em < 20.0);
  -- Lincoln-Petersen: n1 = 20 marked, n2 = 30 records, m recaptures; the prior
  -- N ~ 20 + Poisson(20) (mean 40) puts 20/N near 1/2 (m ~ 15), and the
  -- posterior of N stays near the estimate n1 n2 / m = 20*30/15 = 40.
  SELECT expected(n | (binomial(30, 20.0 / n) = m)) INTO post_n
    FROM (SELECT 20 + poisson(20) AS n) pop, recap_count;
  RAISE NOTICE 'uc3_population_posterior_near_LP: %', (post_n > 28.0 AND post_n < 55.0);
END $$;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;
