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

-- An aggregate of another kind (an avg of a count, a max of a sum) is tracked
-- too: the contribution of each row carries the inner aggregate's own gate
-- (provenance_semimod_nested), so the value of the outer aggregate is read in
-- every world instead of being frozen on the database as it is.  The value
-- shown stays the one plain SQL computes, and the moments are those of the
-- nested aggregate, checked against the 64 worlds of the six rows:
--   avg of the counts: 1.714286 (the average of the present groups' counts,
--     not 3, the average of the counts of the database as it is)
--   max of the sums:   19.126984 (not 32.666667, the max of 35 and 14)
-- The row of a scalar aggregation is always there; the moment is conditional
-- on some group existing, as it is for any min / max / avg.
SELECT avg(c) AS a, round(probability_evaluate(provenance())::numeric, 6) AS p,
       round(expected(avg(c))::numeric, 6) AS e
  FROM (SELECT g, count(*) AS c FROM ra GROUP BY g) x;
SELECT max(s) AS m, round(probability_evaluate(provenance())::numeric, 6) AS p,
       round(expected(max(s))::numeric, 6) AS e
  FROM (SELECT g, sum(v) AS s FROM ra GROUP BY g) x;
-- Grouped above, each answer row carries the provenance of its groups (7/8 for
-- both h, over the three rows each reads) and the expectation of the max of the
-- groups' sums in its worlds: 18.142857 for h=1 (over the groups {10,20} and
-- {7}), 5.142857 for h=2 (over {5} and {3,4}).
CREATE TABLE ra_h AS
  SELECT h, max(s) AS m,
         round(probability_evaluate(provenance())::numeric, 6) AS p,
         round(expected(max(s))::numeric, 6) AS e
  FROM (SELECT g, h, sum(v) AS s FROM ra GROUP BY g, h) x GROUP BY h;
SELECT remove_provenance('ra_h');
SELECT * FROM ra_h ORDER BY h;
DROP TABLE ra_h;

-- A comparison on such an aggregate is resolved by enumerating the worlds of
-- the inputs, which is exact: P(max of the per-world sums > 20) = 0.375, where
-- reading the sums as those of the database as it is would give 0.875, and
-- > 14 holds in 0.625 of the worlds.
CREATE TABLE ra_c AS
  SELECT 20 AS threshold, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM (SELECT g, sum(v) AS s FROM ra GROUP BY g) x HAVING max(s) > 20
  UNION ALL
  SELECT 14, round(probability_evaluate(provenance())::numeric, 6)
  FROM (SELECT g, sum(v) AS s FROM ra GROUP BY g) x HAVING max(s) > 14;
SELECT remove_provenance('ra_c');
SELECT * FROM ra_c ORDER BY threshold;
DROP TABLE ra_c;

DROP TABLE ra;
