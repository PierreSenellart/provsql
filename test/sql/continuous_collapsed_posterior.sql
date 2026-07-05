\set ECHO none
\pset format unaligned

-- Collapsed (Rao-Blackwellised) EXACT posterior of a latent conditioned on a
-- correlated count: R | (Y(R) = C), Y a discrete rv parametrised by R and C a
-- COUNT over selection events sharing one latent.  The per-tuple noise
-- marginalises to the count pmf (a Poisson-binomial 1-D quadrature over the
-- shared latent), and the R posterior is a second 1-D quadrature weighted by
-- L(r) = sum_j P(C=j) pmf_Y(j; theta(r)).  So it is EXACT and works with Monte
-- Carlo DISABLED (rv_mc_samples = 0, where the importance sampler raises),
-- scaling where the point-equality sampler degenerates.  Validated against
-- true importance sampling (which converges to these values) out of band.

SET search_path TO provsql, public;
SET provsql.rv_mc_samples = 0;               -- exact only: the collapse must answer

CREATE TABLE pt(id int, alpha float);
INSERT INTO pt SELECT g, ((g % 7) - 3) FROM generate_series(1, 40) g;
CREATE TABLE bs(b random_variable);
INSERT INTO bs VALUES (normal(0, 0.7));       -- ONE shared classifier bias
CREATE TABLE dg AS
  SELECT p.id, p.alpha, logistic(0, 1) AS eps, b FROM pt p, bs;
SELECT add_provenance('dg');
CREATE TABLE nc AS SELECT count(*) AS c FROM dg WHERE eps < alpha + b;  -- E[C] = 20

DO $$
DECLARE pm double precision; pv double precision; nbm double precision;
BEGIN
  -- Gamma-Poisson: count ~ Poisson(20 R), prior R ~ Gamma(2, 2) (mean 1); the
  -- count's ~20 scale pulls the posterior toward C/20 ~ 1.
  SELECT expected(R | (poisson(20 * R) = c)),
         variance(R | (poisson(20 * R) = c))
    INTO pm, pv FROM (SELECT gamma(2, 2) AS R) g, nc;
  RAISE NOTICE 'gamma_poisson_posterior_mean_exact: %', (abs(pm - 0.960) < 0.02);
  RAISE NOTICE 'gamma_poisson_posterior_var_positive: %', (pv > 0.0 AND pv < 0.2);

  -- Beta-NegativeBinomial (the family-agnostic path): count ~ NB(30, N), prior
  -- N ~ Beta(2, 2); NB(30, p) has mean 30(1-p)/p, so a count ~ 20 identifies
  -- p ~ 0.6.
  SELECT expected(N | (negative_binomial(30, N) = c)) INTO nbm
    FROM (SELECT beta(2, 2) AS N) g, nc;
  RAISE NOTICE 'beta_negbinom_posterior_mean_exact: %', (abs(nbm - 0.603) < 0.02);
END $$;

DROP TABLE pt, bs, dg, nc;
RESET provsql.rv_mc_samples;
