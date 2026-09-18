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

-- Conditioned on its own group (the provenance of the GROUP BY row), the
-- moments of AVG are the unconditional ones, exact: brute force over the 8
-- worlds gives E = 28.877119 and Var = 59.438290.
CREATE TABLE avg_own(g int, id int, x int);
INSERT INTO avg_own VALUES (1,1,10),(1,2,20),(1,3,40);
SELECT add_provenance('avg_own');
SELECT set_prob(provsql, CASE id WHEN 1 THEN 0.3 WHEN 2 THEN 0.6 ELSE 0.8 END)
FROM avg_own \g /dev/null
CREATE TABLE avg_own_r AS
  SELECT avg(x) AS a, provenance() AS p FROM avg_own GROUP BY g;
SET provsql.active = off;
SELECT round(expected(a, p)::numeric, 6) AS e_own,
       round(variance(a, p)::numeric, 6) AS var_own
FROM avg_own_r;
SET provsql.active = on;
DROP TABLE avg_own_r; DROP TABLE avg_own;

-- The group also has a row whose value is NULL: its existence is implied by
-- AVG being defined, the exact route still applies (E = 1.209302 by brute
-- force).  Next to a COUNT(DISTINCT), whose rewrite joins the groups, the
-- rows of AVG stay independent: the exact route too.
CREATE TABLE avg_nul(g int, id int, x int);
INSERT INTO avg_nul VALUES (1,1,2),(1,2,NULL),(1,3,1);
SELECT add_provenance('avg_nul');
SELECT set_prob(provsql, CASE id WHEN 1 THEN 0.3 WHEN 2 THEN 0.1 ELSE 0.8 END)
FROM avg_nul \g /dev/null
CREATE TABLE avg_nul_r AS
  SELECT avg(x) AS a, count(DISTINCT x) AS c, provenance() AS p
  FROM avg_nul GROUP BY g;
SET provsql.active = off;
SELECT round(expected(a, p)::numeric, 6) AS e_null_row,
       round(expected(a)::numeric, 6) AS e_uncond
FROM avg_nul_r;
SET provsql.active = on;
DROP TABLE avg_nul_r; DROP TABLE avg_nul;

SELECT 'ok'::text AS agg_avg_moment_done;
