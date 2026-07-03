\set ECHO none
\pset format unaligned

-- Moments of AVG aggregates: exact over independent rows via the joint
-- (sum, count) distribution (agg_avg_moment_exact, the double-weight
-- instantiation of the HAVING sumCountPMF machinery), conditional on the
-- aggregate being DEFINED (COUNT >= 1: AVG over the empty world is NULL,
-- the MIN/MAX convention).  A laminar shared-root group (join
-- provenance) stays exact; conditioning / non-product shapes fall back
-- to the Monte-Carlo scalar path at the rv_mc_samples budget.

SET provsql.rv_mc_samples = 0;

-- Rows 10 and 100, each present with probability 1/2.  Defined worlds:
-- {10} -> 10, {100} -> 100, {10,100} -> 55, each 1/4:
--   E[avg | defined]  = 165/4 / (3/4) = 55
--   E[avg^2 | defined] = 13125/3 = 4375  ->  Var = 4375 - 55^2 = 1350.
-- Exact, no sampling (rv_mc_samples = 0 throughout).
CREATE TABLE av(g int, x numeric);
INSERT INTO av VALUES (1, 10), (1, 100);
SELECT add_provenance('av');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM av; END $$;
CREATE TABLE avr AS SELECT g, avg(x) AS a FROM av GROUP BY g;
SET provsql.active = off;
SELECT round(expected(a)::numeric, 4) AS e_avg,
       round(variance(a)::numeric, 4) AS var_avg
FROM avr;
SET provsql.active = on;

-- Laminar shared root: each row's provenance is anchored on the same
-- joined tuple (anchor AND row_i).  The joint machinery factors the
-- common leaf out, so the moment stays exact -- and unchanged at 55,
-- since the anchor scales the defined mass and every world uniformly.
CREATE TABLE av_anchor(g int);
INSERT INTO av_anchor VALUES (1);
SELECT add_provenance('av_anchor');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM av_anchor; END $$;
CREATE TABLE avj AS
  SELECT av.g, avg(av.x) AS a
  FROM av JOIN av_anchor USING (g) GROUP BY av.g;
SET provsql.active = off;
SELECT round(expected(a)::numeric, 4) AS e_avg_laminar FROM avj;

-- Conditioning declines the exact arm; without an MC budget the fallback
-- raises the standard actionable error ...
SELECT expected(a, (SELECT provsql FROM av WHERE x = 10)) FROM avr;

-- ... and with a budget it estimates E[avg | row-10 present] =
-- (10 + 55)/2 = 32.5 (worlds {10} and {10,100}, each 1/2 given row 10).
SET provsql.rv_mc_samples = 200000;
SET provsql.monte_carlo_seed = 42;
SELECT abs(expected(a, (SELECT provsql FROM av WHERE x = 10)) - 32.5) < 0.5
       AS e_avg_cond_close
FROM avr;
SET provsql.active = on;

DROP TABLE avj; DROP TABLE av_anchor; DROP TABLE avr; DROP TABLE av;
RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS agg_avg_moment_done;
