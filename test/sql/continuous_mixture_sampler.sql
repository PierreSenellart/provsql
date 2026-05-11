\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Deterministic Monte Carlo with a pinned seed and a large sample
-- budget; we compare empirical / analytical mean / variance within a
-- generous tolerance so the test is robust under platform-dependent
-- RNG state but still catches a sampler that picked the wrong branch
-- or never sampled the Bernoulli at all.
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples    = 50000;

CREATE TEMP TABLE p(t uuid);
INSERT INTO p VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p), 0.3);

-- A.  Mean and variance of a binary GMM M = mixture(p, N(0,1), N(10,1))
--    with π = 0.3.
--      E[M] = 0.3*0 + 0.7*10 = 7.0
--      Var(M) = 0.3*(1 + 0)  + 0.7*(1 + 100) - 49
--             = 0.3 + 70.7 - 49 = 22.0
--             (general formula: π·(Var(X)+E[X]²) + (1-π)·(Var(Y)+E[Y]²) - E[M]²)
CREATE TEMP TABLE mix_a AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.normal(0,  1),
             provsql.normal(10, 1))) AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM mix_a), 1, false) - 7.0)  < 0.1  AS mean_matches,
       abs(provsql.rv_moment((SELECT u FROM mix_a), 2, true)  - 22.0) < 0.5  AS variance_matches;

-- B.  Coupling test: two mixtures sharing the SAME p_token must sample
--     the same branch within one iteration.  Build two well-separated
--     mixtures and check that their sum's variance is the WITHIN-branch
--     variance (each branch concentrates near a single value), not the
--     much larger variance that would result from independent draws of
--     the Bernoulli on each side.
--
--     For independent Bernoullis: each mix has Var ≈ 25 (mean ±5 with
--     prob 0.5 each), so the sum has Var ≈ 50.
--     For shared Bernoulli: both pick the same side, so the sum is
--     either -10 or +10, Var = 100.
SELECT set_prob((SELECT t FROM p), 0.5);

CREATE TEMP TABLE mix_pair AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.as_random(-5),
             provsql.as_random( 5))
         + provsql.mixture(
             (SELECT t FROM p),
             provsql.as_random(-5),
             provsql.as_random( 5))) AS u;

-- The expected coupled variance is 100 (Bernoulli-driven swing across
-- the joint pair).  Allow a wide tolerance since the analytical path
-- may route through MC.
SELECT abs(provsql.rv_moment((SELECT u FROM mix_pair), 2, true) - 100.0) < 5.0
         AS shared_bernoulli_couples_branches;

-- C.  Two mixtures with DIFFERENT p_tokens are independent: same shape,
--     same effective margins, but the Bernoullis are uncorrelated, so
--     the sum spans -10 / 0 / +10 with masses 0.25 / 0.5 / 0.25 and
--     Variance = 50.
CREATE TEMP TABLE p2(t uuid);
INSERT INTO p2 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p2), 0.5);

CREATE TEMP TABLE mix_indep_pair AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.as_random(-5),
             provsql.as_random( 5))
         + provsql.mixture(
             (SELECT t FROM p2),
             provsql.as_random(-5),
             provsql.as_random( 5))) AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM mix_indep_pair), 2, true) - 50.0) < 5.0
         AS distinct_bernoullis_decouple;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
