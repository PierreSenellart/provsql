-- Small mixture-distribution fixture for ProvSQL Studio.  Builds five
-- representative scalar RV roots that exercise every code path the
-- continuous-distribution mixture feature touches:
--
--   * a vanilla bimodal Gaussian mixture (the canonical motivating use
--     case);
--   * a heavy-tailed contamination model (Normal + Normal with different
--     widths);
--   * a mixed-family mixture (Uniform + Exponential), where the closed-form
--     mean / variance still apply but the analytic-CDF fast path does not;
--   * a 3-component cascade built by composition of two binary mixtures;
--   * a coupled pair sharing the same Bernoulli p_token, demonstrating
--     joint sampling.
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_mixture.sql
-- Then point Studio at <db> and try the queries in the trailing comment
-- block below.

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

SET search_path TO public, provsql;

DROP TABLE IF EXISTS mixture_demo CASCADE;

-- A single table of scalar RV roots.  Each row's `expr` column is a
-- random_variable; the `label` describes the shape so the Studio cell
-- view is self-documenting.
CREATE TABLE mixture_demo (
  id     INTEGER PRIMARY KEY,
  label  TEXT NOT NULL,
  expr   provsql.random_variable NOT NULL
);

-- Anchor four Bernoulli p_tokens that we'll reuse across rows.  Stash
-- the UUIDs in a side table so the queries below can read them back.
DROP TABLE IF EXISTS mixture_demo_bernoulli;
CREATE TABLE mixture_demo_bernoulli (
  name  TEXT PRIMARY KEY,
  token UUID NOT NULL,
  pi    FLOAT
);

-- p_50 -- balanced coin, used for the canonical bimodal GMM and a
-- mixture-of-mixtures cascade.
INSERT INTO mixture_demo_bernoulli VALUES
  ('p_50',    public.uuid_generate_v4(), 0.50),
  ('p_50b',   public.uuid_generate_v4(), 0.50),
  ('p_95',    public.uuid_generate_v4(), 0.95),
  ('p_30',    public.uuid_generate_v4(), 0.30);

DO $$
DECLARE r RECORD;
BEGIN
  FOR r IN SELECT name, token, pi FROM mixture_demo_bernoulli LOOP
    PERFORM provsql.create_gate(r.token, 'input');
    PERFORM provsql.set_prob(r.token, r.pi);
  END LOOP;
END $$;

-- Row 1: textbook bimodal Gaussian mixture.
--   M = 0.5·N(-3, 0.5) + 0.5·N(+3, 0.5)
INSERT INTO mixture_demo (id, label, expr)
SELECT 1, '0.5·N(-3, 0.5) + 0.5·N(+3, 0.5) -- bimodal GMM',
       provsql.mixture(token,
                       provsql.normal(-3::float8, 0.5::float8),
                       provsql.normal( 3::float8, 0.5::float8))
  FROM mixture_demo_bernoulli WHERE name = 'p_50';

-- Row 2: heavy-tailed Normal contamination model.
--   95% N(0, 1), 5% N(0, 10) -- mass-equivalent to a unit normal but
--   with the tail bloat of a wide outlier component.
INSERT INTO mixture_demo (id, label, expr)
SELECT 2, '0.95·N(0, 1) + 0.05·N(0, 10) -- heavy-tailed contamination',
       provsql.mixture(token,
                       provsql.normal(0::float8, 1::float8),
                       provsql.normal(0::float8, 10::float8))
  FROM mixture_demo_bernoulli WHERE name = 'p_95';

-- Row 3: mixed-family mixture.  Uniform + Exponential -- both supported
-- by closed-form moments but with different supports.
--   M = 0.3·U(0, 2) + 0.7·Exp(1)
INSERT INTO mixture_demo (id, label, expr)
SELECT 3, '0.3·U(0, 2) + 0.7·Exp(1) -- mixed-family',
       provsql.mixture(token,
                       provsql.uniform(0::float8, 2::float8),
                       provsql.exponential(1::float8))
  FROM mixture_demo_bernoulli WHERE name = 'p_30';

-- Row 4: 3-component cascade.  Built as a nested binary mixture.  The
-- effective weights are (π1, (1-π1)·π2, (1-π1)·(1-π2)) = (0.5, 0.25, 0.25).
--   M = 0.5·N(0, 0.3) + 0.25·N(5, 0.3) + 0.25·N(10, 0.3)
INSERT INTO mixture_demo (id, label, expr)
SELECT 4, '3-component cascade: π=(0.5, 0.25, 0.25) -- N(0,.3)/N(5,.3)/N(10,.3)',
       provsql.mixture((SELECT token FROM mixture_demo_bernoulli WHERE name='p_50'),
                       provsql.normal(0::float8, 0.3::float8),
                       provsql.mixture(
                         (SELECT token FROM mixture_demo_bernoulli WHERE name='p_50b'),
                         provsql.normal( 5::float8, 0.3::float8),
                         provsql.normal(10::float8, 0.3::float8)));

-- Row 5: mixture lifted by an arithmetic shift.  After the simplifier
-- runs, this folds to mixture(p_50, N(3, 1), N(7, 1)) -- the lift
-- pushes the +3 inside the two branches and the normal-family
-- closure collapses each.
--   3 + 0.5·N(0, 1) + 0.5·N(4, 1) -- the lift is what makes this
--   a clean 3+M scalar root rather than an arith tree.
INSERT INTO mixture_demo (id, label, expr)
SELECT 5, '3 + 0.5·N(0,1) + 0.5·N(4,1) -- simplifier lift target',
       provsql.as_random(3) +
       provsql.mixture(token,
                       provsql.normal(0::float8, 1::float8),
                       provsql.normal(4::float8, 1::float8))
  FROM mixture_demo_bernoulli WHERE name = 'p_50';

-- Row 6: coupled-Bernoulli pair (sum of two mixtures sharing p_50).
-- The same Bernoulli draws decide both branches simultaneously, so the
-- joint distribution puts mass on {-10, +10} only (never -5+5 or +5-5).
--   M = mixture(p_50, -5, 5) + mixture(p_50, -5, 5)
INSERT INTO mixture_demo (id, label, expr)
SELECT 6, 'coupled pair (shared p_50): -5/+5 mixtures summed',
       provsql.mixture(token,
                       provsql.as_random(-5),
                       provsql.as_random( 5))
     + provsql.mixture(token,
                       provsql.as_random(-5),
                       provsql.as_random( 5))
  FROM mixture_demo_bernoulli WHERE name = 'p_50';

-- Row 7: decoupled pair (sum of two mixtures with DIFFERENT Bernoullis).
-- Compare with row 6: identical margins, but the joint distribution now
-- spreads across {-10, 0, +10} with masses 0.25 / 0.5 / 0.25.
INSERT INTO mixture_demo (id, label, expr)
SELECT 7, 'decoupled pair (distinct p_50 / p_50b): -5/+5 mixtures summed',
       provsql.mixture(
         (SELECT token FROM mixture_demo_bernoulli WHERE name='p_50'),
         provsql.as_random(-5),
         provsql.as_random( 5))
     + provsql.mixture(
         (SELECT token FROM mixture_demo_bernoulli WHERE name='p_50b'),
         provsql.as_random(-5),
         provsql.as_random( 5));

-- Row 8: ad-hoc probability overload.  No need to pre-mint a Bernoulli
-- token when you don't intend to share it with another circuit branch:
-- the mixture(probability, x, y) overload creates an anonymous
-- gate_input on the fly with the given probability.  Each call mints a
-- fresh Bernoulli, so two calls to mixture(0.5, ...) are independent.
INSERT INTO mixture_demo (id, label, expr)
VALUES (8, '0.7·N(0, 1) + 0.3·N(8, 1) -- ad-hoc probability overload',
        provsql.mixture(0.7::float8,
                        provsql.normal(0::float8, 1::float8),
                        provsql.normal(8::float8, 1::float8)));

\echo 'Loaded 8 mixture rows.  Open Studio against this database and try:'
\echo '  SELECT id, label, expr FROM mixture_demo ORDER BY id;'
\echo 'Then click any random_variable cell to inspect the circuit, and run'
\echo 'the Distribution profile evaluator (try 60 bins on row 1 for the'
\echo 'cleanest bimodal histogram).'
