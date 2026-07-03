\set ECHO none
\pset format unaligned

-- SQL-standard statistic aggregates over random_variable rows:
-- covar_pop / covar_samp / corr (two-argument), stddev_pop / stddev_samp
-- (one-argument), and the ordered-set percentile_cont.  Row presence is
-- carried by a per-row indicator RV; a provenance-tracked query is
-- rewritten by the planner hook to the rv_*_impl aggregates so absent
-- rows drop out of the statistic.  The moments are Monte Carlo (there is
-- no closed form for these compound circuits), but every assertion below
-- on CERTAIN inputs is exact: all draws coincide, so the MC mean is the
-- constant itself.

SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 20000;

-- ---------------------------------------------------------------------
-- Untracked (certain) rows: the public aggregates fold with the certain
-- indicator, and every statistic over Dirac inputs is a Dirac at the
-- classical sample statistic.  x = (1, 3), y = (2, 8):
--   covar_pop = E[xy] - E[x]E[y] = 13 - 10 = 3;   covar_samp = 6;
--   stddev_pop(x) = 1;  stddev_samp(x) = sqrt(2);  corr = 1 (linear).
-- ---------------------------------------------------------------------
CREATE TABLE st(g int, x random_variable, y random_variable);
INSERT INTO st VALUES
  (1, provsql.as_random(1), provsql.as_random(2)),
  (1, provsql.as_random(3), provsql.as_random(8));

SELECT round(expected(covar_pop(x, y))::numeric,  4) AS covar_pop,
       round(expected(covar_samp(x, y))::numeric, 4) AS covar_samp,
       round(expected(corr(x, y))::numeric,       4) AS corr
FROM st GROUP BY g;

SELECT round(expected(stddev_pop(x))::numeric,  4) AS stddev_pop,
       round(expected(stddev_samp(x))::numeric, 4) AS stddev_samp
FROM st GROUP BY g;

-- Draw coupling: corr(x, x) over two independent Normal rows is 1 in
-- every draw (the same per-row draw feeds both argument positions), so
-- the MC mean is exactly 1.
CREATE TABLE stn(g int, x random_variable);
INSERT INTO stn VALUES
  (1, provsql.normal(5, 1)),
  (1, provsql.normal(7, 2));
SELECT round(expected(corr(x, x))::numeric, 4) AS corr_self FROM stn;
DROP TABLE stn;
DROP TABLE st;

-- ---------------------------------------------------------------------
-- Provenance-tracked rows: the planner rewrites to rv_covar_pop_impl &
-- co. with the per-row provenance indicator.  Two rows each present with
-- probability 0.5, x = (1, 3) / y = (2, 8) as above.  Worlds:
--   {}        covar undefined (N = 0) -> NaN, skipped;
--   {r1},{r2} single row -> covar_pop = 0;
--   {r1,r2}   covar_pop = 3.
-- E[covar_pop | defined] = (0 + 0 + 3)/3 = 1.
-- ---------------------------------------------------------------------
CREATE TABLE stp(g int, x random_variable, y random_variable);
INSERT INTO stp VALUES
  (1, provsql.as_random(1), provsql.as_random(2)),
  (1, provsql.as_random(3), provsql.as_random(8));
SELECT add_provenance('stp');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM stp; END $$;

-- Materialise the aggregate results, then read them back untracked so the
-- (run-dependent) group provenance token is not printed.
CREATE TABLE stq AS
  SELECT g, covar_pop(x, y) AS c, stddev_pop(x) AS s FROM stp GROUP BY g;
SET provsql.active = off;
SELECT abs(expected(c) - 1.0) < 0.1 AS covar_pop_tracked_close FROM stq;
-- stddev_pop over the same tracked x = (1, 3): defined worlds
-- {r1} -> 0, {r2} -> 0, {r1,r2} -> 1; E = 1/3.
SELECT abs(expected(s) - 1.0/3) < 0.05 AS stddev_pop_tracked_close FROM stq;
SET provsql.active = on;
DROP TABLE stq; DROP TABLE stp;

-- ---------------------------------------------------------------------
-- percentile_cont over tracked rows.  Certain rows first: the median and
-- the lower quartile of (10, 20, 30) are 20 and 15 in every world, so
-- the MC mean is exact.
-- ---------------------------------------------------------------------
CREATE TABLE pc(g int, x random_variable);
INSERT INTO pc VALUES
  (1, provsql.as_random(10)),
  (1, provsql.as_random(20)),
  (1, provsql.as_random(30));
SELECT add_provenance('pc');
DO $$ BEGIN PERFORM set_prob(provenance(), 1.0) FROM pc; END $$;

CREATE TABLE pct AS
  SELECT g,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY x) AS m,
         percentile_cont(0.25) WITHIN GROUP (ORDER BY x) AS q1
  FROM pc GROUP BY g;
SET provsql.active = off;
SELECT round(expected(m)::numeric, 4)  AS median_certain,
       round(expected(q1)::numeric, 4) AS q1_certain
FROM pct;

-- The gate is the PROVSQL_ARITH_PERCENTILE arith opcode with the fraction
-- in extra.
SELECT get_gate_type(m::uuid) AS pct_gate,
       (get_infos(m::uuid)).info1 AS pct_op,
       get_extra(m::uuid) AS pct_fraction
FROM pct;
SET provsql.active = on;
DROP TABLE pct;

-- Fraction domain validation.
SELECT expected(percentile_cont(1.5) WITHIN GROUP (ORDER BY x))
FROM pc GROUP BY g;
DROP TABLE pc;

-- Uncertain member set: rows 10 and 30 certain, 25 with probability 0.5.
-- Median with the middle row = 25, without = (10+30)/2 = 20; E = 22.5.
CREATE TABLE pcu(g int, x random_variable, w float8);
INSERT INTO pcu VALUES
  (1, provsql.as_random(10), 1.0),
  (1, provsql.as_random(25), 0.5),
  (1, provsql.as_random(30), 1.0);
SELECT add_provenance('pcu');
DO $$ BEGIN PERFORM set_prob(provenance(), w) FROM pcu; END $$;

CREATE TABLE pcm AS
  SELECT g, percentile_cont(0.5) WITHIN GROUP (ORDER BY x) AS m
  FROM pcu GROUP BY g;
SET provsql.active = off;
SELECT abs(expected(m) - 22.5) < 0.2 AS median_uncertain_close FROM pcm;
SET provsql.active = on;
DROP TABLE pcm; DROP TABLE pcu;

RESET provsql.rv_mc_samples;
RESET provsql.monte_carlo_seed;

SELECT 'ok'::text AS stat_aggregates_done;
