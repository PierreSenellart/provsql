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

-- (d) dense_rank would count the distinct counts before the group, which is
-- a DISTINCT on aggregate results: left untracked, with the warning.
CREATE TABLE ro_d AS
  SELECT g, dense_rank() OVER (ORDER BY c) AS dr
  FROM (SELECT g, count(*) AS c FROM ro GROUP BY g) s;
SELECT remove_provenance('ro_d');
SELECT 'dense_rank' AS q, g, dr::text AS dr FROM ro_d ORDER BY g;
DROP TABLE ro_d;

DROP TABLE ro;
