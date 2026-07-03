\set ECHO none
\pset format unaligned

-- Gaussian-mixture-model constructor: provsql.gmm(weights, means, stddevs)
-- packages a categorical choice among Normal components as a stick-breaking
-- cascade of Bernoulli gate_mixture nodes over gate_rv Normal leaves.  The
-- moments ride the exact mixture recursion, so every assertion below runs
-- under rv_mc_samples = 0 (no Monte Carlo).

SET provsql.rv_mc_samples = 0;

-- E = 0.3*120 + 0.5*380 + 0.2*1200 = 466;
-- Var = sum w_i (sigma_i^2 + mu_i^2) - E^2
--     = 0.3*(1600+14400) + 0.5*(8100+144400) + 0.2*(62500+1440000) - 466^2
--     = 381550 - 217156 = 164394.
SELECT round(expected(provsql.gmm(
         weights => ARRAY[0.3, 0.5, 0.2],
         means   => ARRAY[120.0, 380.0, 1200.0],
         stddevs => ARRAY[40.0, 90.0, 250.0]))::numeric, 4) AS e_gmm;
SELECT round(variance(provsql.gmm(
         weights => ARRAY[0.3, 0.5, 0.2],
         means   => ARRAY[120.0, 380.0, 1200.0],
         stddevs => ARRAY[40.0, 90.0, 250.0]))::numeric, 4) AS var_gmm;

-- The mixture CDF is the weighted sum of the component CDFs:
-- P(G < 380) = 0.3*Phi(6.5) + 0.5*Phi(0) + 0.2*Phi(-3.28) ~= 0.5501.
-- Comparisons over a Bernoulli-mixture tree ride the Monte Carlo
-- machinery (the one-at-a-time analytic cmp collapse cannot decompose a
-- mixture without a shared-footprint guard, so it declines).
SET provsql.rv_mc_samples = 200000;
SET provsql.monte_carlo_seed = 7;
SELECT abs(probability(provsql.gmm(
         ARRAY[0.3, 0.5, 0.2],
         ARRAY[120.0, 380.0, 1200.0],
         ARRAY[40.0, 90.0, 250.0]) < 380) - 0.5501) < 0.01 AS p_lt_380_close;
SET provsql.rv_mc_samples = 0;
RESET provsql.monte_carlo_seed;

-- Zero-weight components are skipped; a single positive-weight component
-- returns its Normal directly (a bare gate_rv, no mixture node).
SELECT get_gate_type(provsql.gmm(ARRAY[0.0, 1.0], ARRAY[0.0, 5.0],
                                 ARRAY[1.0, 2.0])::uuid) AS single_component;
SELECT round(expected(provsql.gmm(ARRAY[0.0, 1.0], ARRAY[0.0, 5.0],
                                  ARRAY[1.0, 2.0]))::numeric, 4) AS e_single;

-- A sigma = 0 component degenerates to a Dirac (via provsql.normal).
SELECT round(expected(provsql.gmm(ARRAY[0.5, 0.5], ARRAY[10.0, 20.0],
                                  ARRAY[0.0, 0.0]))::numeric, 4) AS e_dirac_mix;

-- Validation.
SELECT provsql.gmm(ARRAY[0.5, 0.6], ARRAY[0.0, 1.0], ARRAY[1.0, 1.0]);
SELECT provsql.gmm(ARRAY[0.5, 0.5], ARRAY[0.0], ARRAY[1.0, 1.0]);
SELECT provsql.gmm(ARRAY[-0.5, 1.5], ARRAY[0.0, 1.0], ARRAY[1.0, 1.0]);

RESET provsql.rv_mc_samples;

SELECT 'ok'::text AS gmm_done;
