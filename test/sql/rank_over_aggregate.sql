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

-- (f) The values of a sum() are none of its contributions, so the counts of a
-- dense_rank over it cannot be deduplicated: the window is left untracked,
-- with the warning, rather than refused.
CREATE TABLE ro_f AS
  SELECT g, dense_rank() OVER (ORDER BY sum(v)) AS dr FROM ro GROUP BY g;
SELECT remove_provenance('ro_f');
SELECT 'dense_rank over a sum' AS q, g, dr FROM ro_f ORDER BY g;
DROP TABLE ro_f;

DROP TABLE ro;
