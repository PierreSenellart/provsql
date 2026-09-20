\set ECHO none
\pset format unaligned

-- The value of an aggregate is not one value of the database but one per
-- possible world, so grouping rows by it, or deduplicating on it, is no
-- operation on the data as it is: the aggregate is exploded into one row per
-- value it takes, each annotated by the comparison [aggregate = value].  The
-- rows of one group are then pairwise exclusive and exactly one of them is in
-- each world where the group is.

CREATE TABLE eav(g int, v int);
INSERT INTO eav VALUES (1, 10), (1, 20), (2, 30), (3, 40), (3, 50);
SELECT add_provenance('eav');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eav; END $$;

-- GROUP BY on a count: group 1 counts 1 or 2, group 2 counts 1, group 3
-- counts 1 or 2, so the count is 1 unless every group has both its rows
-- (1 - 1/8 = 0.875) and 2 as soon as one of groups 1 and 3 has both
-- (1 - 3/4 * 3/4 = 0.4375).
CREATE TABLE eav_c AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s GROUP BY c;
SELECT remove_provenance('eav_c');
SELECT * FROM eav_c ORDER BY c;
DROP TABLE eav_c;

-- DISTINCT on the same count: the same rows, deduplicated on the value.
CREATE TABLE eav_d AS
  SELECT DISTINCT c FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s;
SELECT remove_provenance('eav_d');
SELECT * FROM eav_d ORDER BY c;
DROP TABLE eav_d;

-- GROUP BY on a max: each value is the maximum of the world where only its
-- own row is (10 with the row of 20 gone: 0.25), and a value that is the
-- largest of its group is the maximum as soon as its row is (0.5).
CREATE TABLE eav_m AS
  SELECT m, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, max(v) AS m FROM eav GROUP BY g) s GROUP BY m;
SELECT remove_provenance('eav_m');
SELECT * FROM eav_m ORDER BY m;
DROP TABLE eav_m;

-- An aggregate over the exploded rows is an aggregate over rows that are
-- uncertain like any others: the displayed value is that of the database as
-- it is (one group of count 1, two groups of count 2), and the expectation is
-- taken over the worlds where the row is.
CREATE TABLE eav_n AS
  SELECT c, count(*)::text AS n,
         round(expected(count(*))::numeric, 6) AS e_n,
         round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, count(*) AS c FROM eav GROUP BY g) s GROUP BY c;
SELECT remove_provenance('eav_n');
SELECT * FROM eav_n ORDER BY c;
DROP TABLE eav_n;

-- A whole-table count takes every value from none of its rows to all of them.
CREATE TABLE eav_s AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT count(*) AS c FROM eav) s GROUP BY c;
SELECT remove_provenance('eav_s');
SELECT * FROM eav_s ORDER BY c;
DROP TABLE eav_s;

-- A DISTINCT over the aggregates of its own level: the aggregation moves to a
-- subquery and the DISTINCT deduplicates the values, giving the rows of the
-- GROUP BY above.
CREATE TABLE eav_sd AS
  SELECT DISTINCT count(*) AS c FROM eav GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_sd ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_sd;

-- The same with the grouping column kept: one row per group and per value it
-- takes, each with the probability of the group taking it.
CREATE TABLE eav_sg AS
  SELECT DISTINCT g, count(*) AS c FROM eav GROUP BY g;
SET provsql.active = off;
SELECT g, c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_sg ORDER BY g, c;
SET provsql.active = on;
DROP TABLE eav_sg;

-- A UNION (non-ALL) of aggregate results: the values of both arms are
-- exploded, and the deduplication is over them.  The second arm counts only
-- the rows over 20, so the value 1 is there unless every group of either arm
-- has all its rows (0.875) and 2 as soon as one of them has two (0.4375).
CREATE TABLE eav_u AS
  SELECT count(*) AS c FROM eav GROUP BY g
  UNION
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_u ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_u;

-- EXCEPT and INTERSECT match the rows they remove or keep on those values,
-- which is why the explosion is done in each arm and not on the result: the
-- second arm counts only the rows over 20, so group 1 (whose rows are 10 and
-- 20) has no counterpart there and its values survive the difference, while
-- groups 2 and 3 count the same rows in both arms and cancel.  Over the 32
-- worlds: 1 with probability 0.125 and 2 with 0.1875 for the difference, 0.75
-- and 0.25 for the intersection.
CREATE TABLE eav_e AS
  SELECT count(*) AS c FROM eav GROUP BY g
  EXCEPT
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_e ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_e;

CREATE TABLE eav_i AS
  SELECT count(*) AS c FROM eav GROUP BY g
  INTERSECT
  SELECT count(*) FROM eav WHERE v > 20 GROUP BY g;
SET provsql.active = off;
SELECT c, round(probability_evaluate(provsql)::numeric, 6) AS pr
FROM eav_i ORDER BY c;
SET provsql.active = on;
DROP TABLE eav_i;

-- A set operation whose other arm aggregates nothing at that column is
-- refused: only the values of a whole column, exploded in every arm, are
-- matched together.
SELECT count(*) AS c FROM eav GROUP BY g UNION SELECT 1;
SELECT count(*) AS c FROM eav GROUP BY g EXCEPT SELECT 1;

-- The values of a sum over an integer column are its subset sums, reached by
-- adding its contributions one at a time: group 1 sums 10 and 20, so it takes
-- 10, 20 or 30, group 2 takes 30, and group 3 takes 40, 50 or 90.  Over the 32
-- worlds each of those is one pair of rows away (0.25), except 30, which two
-- groups reach (0.25 + 0.5 - 0.125 = 0.625).
CREATE TABLE eav_s2 AS
  SELECT total, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s GROUP BY total;
SELECT remove_provenance('eav_s2');
SELECT * FROM eav_s2 ORDER BY total;
DROP TABLE eav_s2;

-- A sum over a column that is not an integer is refused: its value is read
-- back through the evaluator's own arithmetic, that of a double, which a
-- subset sum of such numbers does not reach exactly.  So is an avg(), and any
-- other aggregate whose values cannot be enumerated.
CREATE TABLE eav_n(g int, v numeric);
INSERT INTO eav_n VALUES (1, 0.1), (1, 0.2);
SELECT add_provenance('eav_n');
SELECT total FROM (SELECT g, sum(v) AS total FROM eav_n GROUP BY g) s
  GROUP BY total;
DROP TABLE eav_n;
SELECT a FROM (SELECT g, avg(v) AS a FROM eav GROUP BY g) s GROUP BY a;
SELECT DISTINCT avg(v) AS a FROM eav GROUP BY g;

-- A count() counts the rows whose value is not NULL, and the null-padded row
-- of an outer join is not one of them: the count of such a group is 0 although
-- the group is there, so 0 is one of the values to explode it into.  Here the
-- month 'b' has no right row at all and 'a' has one: over the 8 worlds, 0 is
-- reached whenever 'b' is there or 'a' is there without its right row (0.625),
-- and 1 exactly when both of the latter are (0.25).
CREATE TABLE eavl(m text);
CREATE TABLE eavr(m text, x int);
INSERT INTO eavl VALUES ('a'), ('b');
INSERT INTO eavr VALUES ('a', 1);
SELECT add_provenance('eavl');
SELECT add_provenance('eavr');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM eavl;
        PERFORM set_prob(provenance(), 0.5) FROM eavr; END $$;
CREATE TABLE eav_z AS
  SELECT c, round(probability_evaluate(provenance())::numeric, 6) AS pr
  FROM (SELECT l.m, count(r.x) AS c FROM eavl l LEFT JOIN eavr r ON r.m = l.m
        GROUP BY l.m) s GROUP BY c;
SELECT remove_provenance('eav_z');
SELECT * FROM eav_z ORDER BY c;
DROP TABLE eav_z; DROP TABLE eavl; DROP TABLE eavr;

-- An arm of a set operation that groups by columns it does not expose: those
-- grouping keys are junk entries of the arm, no columns of it, and a row read
-- from one is no row of the range table (it used to crash the planner).  Each
-- group is a single row here, so the count is 1 in every world.
CREATE TABLE eav_j AS
  SELECT count(*) AS c FROM eav WHERE g < 3 GROUP BY g, v
  UNION
  SELECT count(*) FROM eav WHERE g = 3 GROUP BY g, v;
SELECT remove_provenance('eav_j');
SELECT * FROM eav_j ORDER BY c;
DROP TABLE eav_j;

-- The plain value of the sum, said explicitly, groups as plain SQL does.
CREATE TABLE eav_p AS
  SELECT total::numeric AS total FROM (SELECT g, sum(v) AS total FROM eav
  GROUP BY g) s GROUP BY total::numeric;
SELECT remove_provenance('eav_p');
SELECT * FROM eav_p ORDER BY total;
DROP TABLE eav_p;

DROP TABLE eav;
