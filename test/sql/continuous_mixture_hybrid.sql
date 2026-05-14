\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The HybridSimplifier's try_mixture_lift rule pushes PLUS / TIMES
-- inside a single mixture child, then recursively folds the two new
-- branches via try_normal_closure / try_erlang_closure when applicable.
--
-- We can't observe the simplified circuit directly from SQL (the
-- simplifier runs inside getGenericCircuit's in-memory pass), but we
-- can verify the closed-form moment outputs are EXACT -- a value the
-- MC fallback could not match within the floating-point tolerances
-- below -- which only happens when the lift fired AND the inner
-- closures collapsed each branch to a single normal.

SET provsql.monte_carlo_seed = 13;
SET provsql.rv_mc_samples    = 100;     -- low budget: MC can't satisfy 1e-9

CREATE TEMP TABLE p(t uuid);
INSERT INTO p VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p), 0.3);

-- A.  Additive lift: 3 + mixture(p, N(0,1), N(2,1)).
--     After lift + normal closure, the simplifier sees
--       mixture(p, N(3,1), N(5,1))
--     and the closed-form mean is 0.3·3 + 0.7·5 = 4.4, variance is
--       0.3·(1+9) + 0.7·(1+25) - 4.4² = 3 + 18.2 - 19.36 = 1.84.
CREATE TEMP TABLE m_add AS
  SELECT (
           provsql.as_random(3) +
           provsql.mixture((SELECT t FROM p),
                           provsql.normal(0, 1),
                           provsql.normal(2, 1)))::uuid AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_add), 1, false) - 4.4)  < 1e-9 AS add_lift_mean,
       abs(provsql.rv_moment((SELECT u FROM m_add), 2, true)  - 1.84) < 1e-9 AS add_lift_variance;

-- B.  Multiplicative lift: 2 * mixture(p, N(0,1), N(2,1)).
--     After lift + normal closure: mixture(p, N(0,2), N(4,2)).
--       E[M] = 0.3·0 + 0.7·4 = 2.8
--       Var(M) = 0.3·(4+0) + 0.7·(4+16) - 2.8² = 1.2 + 14 - 7.84 = 7.36.
CREATE TEMP TABLE m_mul AS
  SELECT (
           provsql.as_random(2) *
           provsql.mixture((SELECT t FROM p),
                           provsql.normal(0, 1),
                           provsql.normal(2, 1)))::uuid AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_mul), 1, false) - 2.8 ) < 1e-9 AS mul_lift_mean,
       abs(provsql.rv_moment((SELECT u FROM m_mul), 2, true)  - 7.36) < 1e-9 AS mul_lift_variance;

-- C.  Heterogeneous branches: PLUS over a constant and a mixture of
--     a uniform + an exponential.  The lift still fires (only one
--     mixture child), but the new branches are non-foldable arith
--     (uniform + 3, exponential + 3) so the rec_* dispatcher falls
--     through to the per-branch arith analysis.
--       X = U(0,2) + 3 → support (3, 5),    E = 4,    Var = 4/12.
--       Y = Exp(1) + 3 → support (3, ∞),    E = 4,    Var = 1.
--       E[M] = 0.4·4 + 0.6·4 = 4.0
--       Var(M) = 0.4·(1/3 + 16) + 0.6·(1 + 16) - 16
--              = 0.4·(16.333) + 0.6·17 - 16 = 6.533 + 10.2 - 16 = 0.733
SELECT set_prob((SELECT t FROM p), 0.4);
CREATE TEMP TABLE m_het AS
  SELECT (
           provsql.as_random(3) +
           provsql.mixture((SELECT t FROM p),
                           provsql.uniform(0, 2),
                           provsql.exponential(1)))::uuid AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_het), 1, false) - 4.0) < 1e-9 AS het_lift_mean;
-- Variance value: 0.4*(1/3 + 16) + 0.6*(1 + 16) - 16 = 0.7333...
SELECT abs(provsql.rv_moment((SELECT u FROM m_het), 2, true)
        - (0.4 * (1.0/3.0 + 16.0) + 0.6 * (1.0 + 16.0) - 16.0)) < 1e-9
         AS het_lift_variance;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
