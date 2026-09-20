\set ECHO none
\pset format unaligned

-- An aggregate of an aggregate result of the same kind: sum over a sum or a
-- count, max over a max, min over a min, is the aggregate of the rows of the
-- groups (their contributions, scaled by the provenance of the group row: a
-- semimodule scalar multiplication, in every semiring); count over a count
-- counts the groups.  Checked against a count over the 64 worlds of the six
-- rows, each present with probability 1/2.

CREATE TABLE ra(g int, h int, v int);
INSERT INTO ra VALUES (1,1,10),(1,1,20),(1,2,5),(2,1,7),(2,2,3),(2,2,4);
SELECT add_provenance('ra');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM ra; END $$;

CREATE TABLE ra_r AS
  SELECT 'sum of sums' AS q, g, sum(s)::text AS r,
         round(probability_evaluate(provenance())::numeric, 6) AS p,
         round(expected(sum(s))::numeric, 6) AS e
  FROM (SELECT g, h, sum(v) AS s FROM ra GROUP BY g, h) x GROUP BY g
  UNION ALL
  SELECT 'sum of counts', g, sum(c)::text,
         round(probability_evaluate(provenance())::numeric, 6),
         round(expected(sum(c))::numeric, 6)
  FROM (SELECT g, h, count(*) AS c FROM ra GROUP BY g, h) x GROUP BY g
  UNION ALL
  SELECT 'max of maxes', g, max(m)::text,
         round(probability_evaluate(provenance())::numeric, 6),
         round(expected(max(m))::numeric, 6)
  FROM (SELECT g, h, max(v) AS m FROM ra GROUP BY g, h) x GROUP BY g
  UNION ALL
  SELECT 'min of mins', g, min(m)::text,
         round(probability_evaluate(provenance())::numeric, 6),
         round(expected(min(m))::numeric, 6)
  FROM (SELECT g, h, min(v) AS m FROM ra GROUP BY g, h) x GROUP BY g
  UNION ALL
  SELECT 'count of counts', g, count(c)::text,
         round(probability_evaluate(provenance())::numeric, 6),
         round(expected(count(c))::numeric, 6)
  FROM (SELECT g, h, count(*) AS c FROM ra GROUP BY g, h) x GROUP BY g;
SELECT remove_provenance('ra_r');
SELECT * FROM ra_r ORDER BY q, g;
DROP TABLE ra_r;

-- An aggregate of another kind (an avg of a count, a max of a sum) reads the
-- inner value on the database as it is, as an explicit ::numeric on the inner
-- aggregate would, and the loss is reported.  The outer aggregate is tracked
-- over the rows of the groups with those values: a scalar aggregation always
-- yields a row, and its expectation is over the worlds where some group
-- exists (63 of the 64).  Both match a count over the 64 worlds: avg of the
-- counts (3, 3) is 3 wherever a group exists, max of the sums (35, 14) is
-- 35 with probability 7/8 and 14 with probability 7/64, so 32.666667.
SELECT avg(c) AS a, round(probability_evaluate(provenance())::numeric, 6) AS p,
       round(expected(avg(c))::numeric, 6) AS e
  FROM (SELECT g, count(*) AS c FROM ra GROUP BY g) x;
SELECT max(s) AS m, round(probability_evaluate(provenance())::numeric, 6) AS p,
       round(expected(max(s))::numeric, 6) AS e
  FROM (SELECT g, sum(v) AS s FROM ra GROUP BY g) x;
-- Grouped above, each answer row carries the provenance of its groups: h=1
-- reads the groups of sums 30 and 7, present with probability 3/4 and 1/2,
-- h=2 those of sums 5 and 7, so each row is there with probability 7/8 and
-- the max is 26.714286 in expectation (23.375 over the 7/8).
CREATE TABLE ra_h AS
  SELECT h, max(s) AS m,
         round(probability_evaluate(provenance())::numeric, 6) AS p,
         round(expected(max(s))::numeric, 6) AS e
  FROM (SELECT g, h, sum(v) AS s FROM ra GROUP BY g, h) x GROUP BY h;
SELECT remove_provenance('ra_h');
SELECT * FROM ra_h ORDER BY h;
DROP TABLE ra_h;

DROP TABLE ra;
