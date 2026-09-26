\set ECHO none
\pset format unaligned

-- The rank of a group by one of its aggregates: the value it ranks on varies
-- between worlds, so the groups before a group do too.  The rank is read as
-- the number of groups before it, itself included, which compares the two
-- aggregate results per pair of groups.  Checked against a count over the 32
-- worlds of the five rows, each present with probability 1/2:
--   group 1 (two rows) and group 3 (two rows): E[rank] = 1.166667, and each
--   is in the top 2 with probability 0.75;
--   group 2 (one row): E[rank] = 1.5, in the top 2 with probability 0.46875.

CREATE TABLE ro(g int, v int);
INSERT INTO ro VALUES (1,10), (1,20), (2,5), (3,7), (3,8);
SELECT add_provenance('ro');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ro; END $$;

-- (a) A window over an aggregate column of a subquery.
CREATE TABLE ro_a AS
  SELECT g, c, rank() OVER (ORDER BY c DESC) AS rk
  FROM (SELECT g, count(*) AS c FROM ro GROUP BY g) s;
SET provsql.active = off;
SELECT 'window over a subquery' AS q, g, c::text AS c, rk::text AS rk,
       round(expected(rk, provsql)::numeric, 6) AS e_rank
FROM ro_a ORDER BY g;
SET provsql.active = on;

-- (b) The same window over the aggregates of its own query level: the
-- aggregation moves to a subquery, and the tokens are those of (a).
CREATE TABLE ro_b AS
  SELECT g, count(*) AS c, rank() OVER (ORDER BY count(*) DESC) AS rk
  FROM ro GROUP BY g;
SET provsql.active = off;
SELECT 'window over its own aggregates' AS q, g, c::text AS c, rk::text AS rk,
       round(expected(rk, provsql)::numeric, 6) AS e_rank
FROM ro_b ORDER BY g;
SELECT 'same tokens as (a)' AS q,
       bool_and(a.provsql = b.provsql) AS same
FROM ro_a a JOIN ro_b b USING (g);
SET provsql.active = on;
DROP TABLE ro_a; DROP TABLE ro_b;

-- (b2) The same, with the output ordered: an ORDER BY of the query (here on a
-- grouping column) stays with it, and the window is still tracked.  It used
-- to make the window untracked, read as plain SQL with a warning.
CREATE TABLE ro_b2 AS
  SELECT g, count(*) AS c, rank() OVER (ORDER BY count(*) DESC) AS rk
  FROM ro GROUP BY g ORDER BY g;
SET provsql.active = off;
SELECT 'window over its own aggregates, ordered' AS q, g, c::text AS c,
       rk::text AS rk, round(expected(rk, provsql)::numeric, 6) AS e_rank
FROM ro_b2 ORDER BY g;
SET provsql.active = on;
DROP TABLE ro_b2;

-- (c) The top two groups: the LIMIT is the filter of that rank, so a group
-- is kept in the worlds where it is among the first two.
CREATE TABLE ro_c AS
  SELECT g, count(*) AS c FROM ro GROUP BY g ORDER BY count(*) DESC LIMIT 2;
SET provsql.active = off;
SELECT 'top 2' AS q, g, c::text AS c,
       round(probability_evaluate(provsql)::numeric, 6) AS p
FROM ro_c ORDER BY g;
SET provsql.active = on;
DROP TABLE ro_c;

-- (d) dense_rank counts the distinct counts up to the group's own, a DISTINCT
-- on aggregate results: the counts are exploded into one row per value they
-- take, deduplicated once over the whole relation, and counted.  Over the 32
-- worlds: the two-row groups have E[dense_rank] = 1.25, and the one-row group
-- 1, its count being the smallest whenever the group is there.
CREATE TABLE ro_d AS
  SELECT g, dense_rank() OVER (ORDER BY c) AS dr
  FROM (SELECT g, count(*) AS c FROM ro GROUP BY g) s;
SET provsql.active = off;
SELECT 'dense_rank' AS q, g, dr::text AS dr,
       round(expected(dr, provsql)::numeric, 6) AS e_dense
FROM ro_d ORDER BY g;
SET provsql.active = on;
DROP TABLE ro_d;

-- (e) dense_rank per partition: the keys are deduplicated once, over every
-- partition at once, and counted within the partition of the row.  Over the
-- 64 worlds of the six rows, E[dense_rank] = 1.166667 for the two-row groups
-- and 1 for the one-row ones.
CREATE TABLE rop(part text, g int);
INSERT INTO rop VALUES ('x',1), ('x',1), ('x',2), ('y',3), ('y',3), ('y',4);
SELECT add_provenance('rop');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM rop; END $$;
CREATE TABLE rop_d AS
  SELECT part, g, dense_rank() OVER (PARTITION BY part ORDER BY count(*)) AS dr
  FROM rop GROUP BY part, g;
SET provsql.active = off;
SELECT 'dense_rank per partition' AS q, part, g, dr::text AS dr,
       round(expected(dr, provsql)::numeric, 6) AS e_dense
FROM rop_d ORDER BY part, g;
SET provsql.active = on;
DROP TABLE rop_d; DROP TABLE rop;

-- (f) A sum() over an integer column takes its subset sums, which can be
-- enumerated, so a dense_rank over it is tracked like one over a count.  Over
-- the 32 worlds: E[dense_rank] = 2.166667 for group 1 (sums 10, 20 or 30),
-- 1 for group 2 (its 5 is the smallest sum whenever it is there) and 1.583333
-- for group 3 (7, 8 or 15).
CREATE TABLE ro_f AS
  SELECT g, dense_rank() OVER (ORDER BY sum(v)) AS dr FROM ro GROUP BY g;
SET provsql.active = off;
SELECT 'dense_rank over a sum' AS q, g, dr::text AS dr,
       round(expected(dr, provsql)::numeric, 6) AS e_dense
FROM ro_f ORDER BY g;
SET provsql.active = on;
DROP TABLE ro_f;

-- (g) The ordering key of a LIMIT over an aggregation may be arithmetic over
-- an aggregate, not only a bare one: the rank is the filter of that order all
-- the same.  count(*) + 1 keeps the order of the count, so the top two are the
-- two-row groups with probability 0.75 and the one-row group with 0.46875, as
-- in (c).
CREATE TABLE ro_g AS
  SELECT g, count(*) + 1 AS c FROM ro GROUP BY g ORDER BY count(*) + 1 DESC
  LIMIT 2;
SET provsql.active = off;
SELECT 'top 2 by an arithmetic key' AS q, g, c::text AS c,
       round(probability_evaluate(provsql)::numeric, 6) AS p
FROM ro_g ORDER BY g;
SET provsql.active = on;
DROP TABLE ro_g;

-- The relation ranked is a VIEW: expanded into a subquery, it keeps the
-- permission entry of the view, which the subquery counting the rows before
-- each one must carry with its copy of the view, as it does for a table's
-- ("invalid perminfoindex", difftest's W16 on SQLShare).  Two rows of 1, one
-- of 2, each at one half: 1 is first wherever it has a row (0.75), 2 second.
CREATE TABLE ro_vt(k int);
INSERT INTO ro_vt VALUES (1), (1), (2);
SELECT add_provenance('ro_vt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ro_vt; END $$;
CREATE VIEW ro_v AS SELECT k, count(*) AS c FROM ro_vt GROUP BY k;
CREATE TABLE ro_vr AS
  SELECT k, rank() OVER (ORDER BY c DESC) AS r,
         round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM ro_v;
SELECT remove_provenance('ro_vr');
SELECT k, r::text AS r, p FROM ro_vr ORDER BY k;
DROP TABLE ro_vr;
DROP VIEW ro_v;
SELECT remove_provenance('ro_vt');
DROP TABLE ro_vt;

-- The rank of a row whose grouping columns are all NULL, the grand total of
-- a ROLLUP (TPC-DS 67).  The subquery counting the rows before and the row
-- itself keyed its count on a grouping column, NULL on that row too, and
-- counted no row at all: rank 0.  It is keyed on a constant now.  Over an empty
-- table the grand total is the one row, of rank 1 in every world.
CREATE TABLE ro_e(c text, x int);
SELECT add_provenance('ro_e');
CREATE TABLE ro_er AS
  SELECT c, rank() OVER (ORDER BY s DESC) AS rk
  FROM (SELECT c, sum(x) AS s FROM ro_e GROUP BY ROLLUP(c)) d;
SELECT remove_provenance('ro_er');
SELECT c, rk::text AS rk FROM ro_er;
DROP TABLE ro_er;
SELECT remove_provenance('ro_e');
DROP TABLE ro_e;

-- A dense_rank() ordered by a CASE over the aggregates of its own query is
-- computed on the plain values, with a warning; the CASE lowered to an
-- agg_token inside what the dense_rank counts was compared as one, and raised
-- ("Comparison agg_token-agg_token not implemented").  The ranks are plain
-- SQL's.
CREATE TABLE ro_c(pid int);
INSERT INTO ro_c VALUES (1), (1), (2), (3), (3), (3);
SELECT add_provenance('ro_c');
SET client_min_messages = error;
CREATE TABLE ro_cr AS
  SELECT pid, dense_rank() OVER (ORDER BY CASE WHEN count(*) > 1
                                              THEN count(*) ELSE 0 END DESC) AS r
  FROM ro_c GROUP BY pid;
RESET client_min_messages;
SELECT remove_provenance('ro_cr');
SELECT pid, r::text AS r FROM ro_cr ORDER BY pid;
DROP TABLE ro_cr;
SELECT remove_provenance('ro_c');
DROP TABLE ro_c;

-- A rank by an aggregate that is NULL in some world (a sum over values that
-- are all NULL): the NULL sorts where the ORDER BY puts NULLs, first under
-- DESC and last under ASC, and two NULLs tie, as SQL has it.  The comparison
-- alone left the NULL out, silently (difftest's W21: under DESC, 3 came first
-- beside a NULL).  Rows at one half; the probabilities of rank 1 are those of
-- the enumeration of the 32 worlds.
CREATE TABLE ro_n(id int, k int, x int);
INSERT INTO ro_n VALUES (1, 1, NULL), (2, 1, 4), (3, 2, 3), (4, 2, NULL),
                        (5, 3, 5);
SELECT add_provenance('ro_n');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ro_n; END $$;
CREATE TABLE ro_nr AS
  SELECT 'rank DESC' AS q, k,
         round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT k, rank() OVER (ORDER BY s DESC) AS r
        FROM (SELECT k, sum(x) AS s FROM ro_n GROUP BY k) d) z
  WHERE r = 1
  UNION ALL
  SELECT 'dense_rank ASC NULLS FIRST', k,
         round(probability_evaluate(provenance())::numeric, 6)
  FROM (SELECT k, dense_rank() OVER (ORDER BY s ASC NULLS FIRST) AS r
        FROM (SELECT k, sum(x) AS s FROM ro_n GROUP BY k) d) z
  WHERE r = 1;
SELECT remove_provenance('ro_nr');
SELECT q, k, p FROM ro_nr ORDER BY q, k;
DROP TABLE ro_nr;
SELECT remove_provenance('ro_n');
DROP TABLE ro_n;

DROP TABLE ro;
