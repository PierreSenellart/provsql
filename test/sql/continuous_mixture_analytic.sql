\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Closed-form moments for a binary mixture must compose recursively
-- through Expectation.cpp's rec_* dispatchers.  Pin a seed so the MC
-- fallback (if accidentally triggered) is reproducible, but the
-- assertions are tight enough that the analytical path is the only
-- way to satisfy them.
SET provsql.monte_carlo_seed = 1;
SET provsql.rv_mc_samples    = 1000;

CREATE TEMP TABLE p(t uuid);
INSERT INTO p VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p), 0.4);

-- A.  Two-component normal mixture with mismatched widths.
--      M = mixture(0.4 → N(0,1), 0.6 → N(5,2))
--      E[M] = 0.4·0 + 0.6·5 = 3.0
--      Var(M) = 0.4·(1+0) + 0.6·(4+25) - 9.0
--             = 0.4 + 17.4 - 9.0 = 8.8
--      E[M^3] = 0.4·E[X^3] + 0.6·E[Y^3]
--             X ~ N(0,1): odd central moments vanish, raw moment 3 is
--                         μ³ + 3μσ² = 0 for μ = 0.
--             Y ~ N(5,2): raw_moment(3) = μ³ + 3μσ² = 125 + 3·5·4 = 185.
--             So E[M^3] = 0.4·0 + 0.6·185 = 111.0.
CREATE TEMP TABLE m_normal AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.normal(0, 1),
             provsql.normal(5, 2))) AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_normal), 1, false) - 3.0  ) < 1e-9 AS exact_mean,
       abs(provsql.rv_moment((SELECT u FROM m_normal), 2, true)  - 8.8  ) < 1e-9 AS exact_variance,
       abs(provsql.rv_moment((SELECT u FROM m_normal), 3, false) - 111.0) < 1e-9 AS exact_third_moment;

-- B.  Mixed-family mixture: uniform and exponential branches.
--      M = mixture(0.4 → U(0,2), 0.6 → Exp(1))
--      E[U(0,2)] = 1; Var(U(0,2)) = 4/12 = 1/3.
--      E[Exp(1)] = 1; Var(Exp(1)) = 1.
--      E[M] = 0.4·1 + 0.6·1 = 1.0
--      Var(M) = 0.4·(1/3 + 1) + 0.6·(1 + 1) - 1.0
--             = 0.4·4/3 + 1.2 - 1.0 = 0.5333... + 0.2 = 0.7333...
SELECT set_prob((SELECT t FROM p), 0.4);
CREATE TEMP TABLE m_mixed AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.uniform(0, 2),
             provsql.exponential(1))) AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_mixed), 1, false) - 1.0) < 1e-9 AS mixed_mean,
       abs(provsql.rv_moment((SELECT u FROM m_mixed), 2, true)  - (0.4 * (1.0/3.0) + 0.6 * 1.0 + (0.4 * 1 + 0.6 * 1) - (0.4 * 1 + 0.6 * 1)*(0.4 * 1 + 0.6 * 1) ))
                                                                  < 1e-9 AS mixed_variance;
-- The wickedly-parenthesised expected variance simplifies to
--   0.4 * (Var_U + E_U²) + 0.6 * (Var_E + E_E²) - (0.4·E_U + 0.6·E_E)²
-- = 0.4 * (1/3 + 1)        + 0.6 * (1 + 1)       - (1)²
-- = 0.4 * 4/3 + 1.2 - 1
-- ≈ 0.73333

-- C.  Nested mixture (mixture-of-mixtures).  Effective 3-component
--     mixture with weights (π1, (1-π1)·π2, (1-π1)·(1-π2)).
--      Outer p1 = 0.5, inner p2 = 0.5 → effective weights 0.5/0.25/0.25.
--      Components: as_random(0), as_random(2), as_random(10).
--      E[M] = 0.5·0 + 0.25·2 + 0.25·10 = 3.0
--      E[M²]= 0.5·0 + 0.25·4 + 0.25·100 = 26.0
--      Var(M) = 26 - 9 = 17.0
CREATE TEMP TABLE p1(t uuid);
INSERT INTO p1 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p1), 0.5);
CREATE TEMP TABLE p2(t uuid);
INSERT INTO p2 VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p2), 0.5);

CREATE TEMP TABLE m_nested AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p1),
             provsql.as_random(0),
             provsql.mixture(
               (SELECT t FROM p2),
               provsql.as_random(2),
               provsql.as_random(10)))) AS u;

SELECT abs(provsql.rv_moment((SELECT u FROM m_nested), 1, false) - 3.0)  < 1e-9 AS nested_mean,
       abs(provsql.rv_moment((SELECT u FROM m_nested), 2, true)  - 17.0) < 1e-9 AS nested_variance;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
