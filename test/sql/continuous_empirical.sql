\set ECHO none
\pset format unaligned

-- Empirical / tabulated distributions: empirical_samples (the ecdf of a
-- sample bundle, reduced to a categorical) and empirical_cdf (a
-- piecewise-linear CDF table, reduced to a mixture of uniforms plus an
-- optional atom).  The discrete surface and the mixture moments are
-- exact, so everything below runs under rv_mc_samples = 0.

SET provsql.rv_mc_samples = 0;

-- ---------------------------------------------------------------------
-- empirical_samples([1, 2, 2, 3]): mass 1/4, 1/2, 1/4.
-- E = 2; Var = 0.5; P(X <= 2) = 0.75 (analytic, "fraction of samples
-- below c"); median = 2 (exact empirical quantile); entropy =
-- -(1/4 ln 1/4 + 1/2 ln 1/2 + 1/4 ln 1/4) ~= 1.0397.
-- ---------------------------------------------------------------------
SELECT round(expected(provsql.empirical_samples(
         ARRAY[1.0, 2.0, 2.0, 3.0]))::numeric, 4) AS e_samples;
SELECT round(variance(provsql.empirical_samples(
         ARRAY[1.0, 2.0, 2.0, 3.0]))::numeric, 4) AS var_samples;
SELECT round(probability(provsql.empirical_samples(
         ARRAY[1.0, 2.0, 2.0, 3.0]) <= 2)::numeric, 4) AS p_le_2;
SELECT round(quantile(provsql.empirical_samples(
         ARRAY[1.0, 2.0, 2.0, 3.0]), 0.5)::numeric, 4) AS median_samples;
SELECT round(entropy(provsql.empirical_samples(
         ARRAY[1.0, 2.0, 2.0, 3.0]))::numeric, 4) AS h_samples;

-- A single distinct value degenerates to a Dirac (gate_value).
SELECT get_gate_type(provsql.empirical_samples(
         ARRAY[5.0, 5.0, 5.0])::uuid) AS single_value;

-- Validation.
SELECT provsql.empirical_samples(ARRAY[]::float8[]);
SELECT provsql.empirical_samples(ARRAY[1.0, 'NaN'::float8]);

-- ---------------------------------------------------------------------
-- empirical_cdf.  grid [0,1,2], cdf [0, 0.5, 1]: no atom, two uniform
-- pieces of mass 0.5 each -> E = 1, Var = E[X^2] - 1 = 4/3 - 1 = 1/3.
-- grid [0,2], cdf [0.25, 1]: atom of 0.25 at 0 + U(0,2) of 0.75 ->
-- E = 0.75, support [0, 2].
-- ---------------------------------------------------------------------
SELECT round(expected(provsql.empirical_cdf(
         ARRAY[0.0, 1.0, 2.0], ARRAY[0.0, 0.5, 1.0]))::numeric, 4)
       AS e_cdf;
SELECT round(variance(provsql.empirical_cdf(
         ARRAY[0.0, 1.0, 2.0], ARRAY[0.0, 0.5, 1.0]))::numeric, 4)
       AS var_cdf;
SELECT round(expected(provsql.empirical_cdf(
         ARRAY[0.0, 2.0], ARRAY[0.25, 1.0]))::numeric, 4) AS e_cdf_atom;
SELECT round(lo::numeric, 4) AS supp_lo, round(hi::numeric, 4) AS supp_hi
  FROM provsql.rv_support(provsql.empirical_cdf(
         ARRAY[0.0, 2.0], ARRAY[0.25, 1.0])::uuid) AS s(lo, hi);

-- The design example: a rainfall forecast table.
SELECT round(expected(provsql.empirical_cdf(
         grid => ARRAY[0.0, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0],
         cdf  => ARRAY[0.32, 0.51, 0.67, 0.82, 0.94, 0.99, 1.0]))::numeric, 4)
       AS e_rainfall;

-- Validation.
SELECT provsql.empirical_cdf(ARRAY[0.0, 1.0], ARRAY[0.5, 0.9]);
SELECT provsql.empirical_cdf(ARRAY[1.0, 0.0], ARRAY[0.5, 1.0]);
SELECT provsql.empirical_cdf(ARRAY[0.0, 1.0], ARRAY[0.9, 0.5]);
SELECT provsql.empirical_cdf(ARRAY[0.0], ARRAY[1.0]);

RESET provsql.rv_mc_samples;

SELECT 'ok'::text AS empirical_done;
