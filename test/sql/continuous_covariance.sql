\set ECHO none
\pset format unaligned

-- Covariance / correlation / stddev: the bivariate readouts complementing
-- the univariate moment surface.  covariance(X,Y)=E[XY]-E[X]E[Y] with exact
-- tiers (identical roots -> variance; disjoint stochastic-leaf footprints ->
-- exact 0; every factor analytic -> closed-form subtraction) and a single
-- coupled MC pass over (x, y) pairs otherwise; correlation reads cov and
-- both variances off the same pass.  stddev=sqrt(variance).

SET provsql.monte_carlo_seed = 1;
SET provsql.rv_mc_samples = 200000;
SET search_path TO provsql, public;

-- 1. Independent RVs: disjoint gate_rv footprints give E[XY]=E[X]*E[Y], so
--    covariance and correlation are EXACT 0 (each normal() / uniform() call
--    mints a fresh, independent leaf).
SELECT abs(covariance(normal(0,1), normal(0,1)))   < 1e-9 AS indep_cov_zero,
       abs(correlation(normal(2,3), uniform(0,4))) < 1e-9 AS indep_corr_zero;

-- 2. stddev = sqrt(Var), exact on a single analytic leaf.
SELECT abs(stddev(normal(2,3))      - 3.0)            < 1e-9 AS sd_normal,   -- sigma
       abs(stddev(uniform(0,1))     - sqrt(1.0/12.0)) < 1e-9 AS sd_uniform,  -- (b-a)/sqrt(12)
       abs(stddev(exponential(2.0)) - 0.5)            < 1e-9 AS sd_exp;      -- 1/lambda

-- 3. Shared leaf: Cov(X,X)=Var(X)=1 and Corr(X,X)=1 (identical roots route
--    to the variance evaluator, closed-form for a bare normal leaf).
--    Reusing the same gate_rv UUID couples the two arguments perfectly.
WITH a AS (SELECT (normal(0,1))::uuid u)
SELECT abs(covariance(random_variable_make(u), random_variable_make(u)) - 1.0) < 0.05
         AS cov_self_is_var,
       abs(correlation(random_variable_make(u), random_variable_make(u)) - 1.0) < 0.05
         AS corr_self_is_one
  FROM a;

-- 4. Degenerate: a constant has stddev 0, so correlation is undefined and
--    returns NULL rather than raising a division-by-zero.
SELECT stddev(as_random(5.0)) = 0                       AS sd_const_zero,
       correlation(normal(0,1), as_random(5.0)) IS NULL AS corr_const_null;

-- 5. Conditioning: X,Y ~ U(0,1) conditioned on X+Y<=1 (the uniform law over
--    the lower triangle) has Cov = -1/36 (MC-backed).  The conditioning
--    event is the row provenance; materialise + strip it so the readout is
--    a plain scalar.
CREATE TABLE cov_tri AS
  SELECT covariance(x, y, provenance()) AS c
    FROM (SELECT uniform(0,1) AS x, uniform(0,1) AS y) t
   WHERE x + y <= 1;
SELECT remove_provenance('cov_tri');
SELECT abs(c - (-1.0/36.0)) < 0.01 AS tri_cov_negative FROM cov_tri;
DROP TABLE cov_tri;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
