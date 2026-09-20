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

-- The values of a sum are its subset sums, which are not read off its
-- contributions one by one: refused, as is grouping by the value of any
-- aggregate other than count(), min(), max() and choose().
SELECT total FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s
  GROUP BY total;
SELECT DISTINCT total FROM (SELECT g, sum(v) AS total FROM eav GROUP BY g) s;

-- The plain value of the sum, said explicitly, groups as plain SQL does.
CREATE TABLE eav_p AS
  SELECT total::numeric AS total FROM (SELECT g, sum(v) AS total FROM eav
  GROUP BY g) s GROUP BY total::numeric;
SELECT remove_provenance('eav_p');
SELECT * FROM eav_p ORDER BY total;
DROP TABLE eav_p;

DROP TABLE eav;
