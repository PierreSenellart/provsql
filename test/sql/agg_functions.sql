\set ECHO none
\pset format unaligned

-- ----------------------------------------------------------------------
-- round, floor, ceil and abs of an aggregate result are carried as gate
-- operations (PROVSQL_ARITH_ROUND / FLOOR / CEIL / ABS), so the value is read
-- in every possible world instead of being frozen on the database as it is.
-- The moments below are conditional on the group existing, and every one of
-- them is a count over the worlds of the rows (asymmetric probabilities, so
-- that no symmetry can make a wrong reading look right):
--   g=1: 10 at 0.3, 20 at 0.5, 5 at 0.7
--   g=2: 7 at 0.4, 3 at 0.6
--   g=3: 4 at 0.8
-- ----------------------------------------------------------------------

CREATE TABLE af_t(g int, v int);
-- Group 4 holds a negative value so that abs() is not the identity on any of
-- the groups it is read over: with every value positive, a gate that returned
-- its input unchanged would answer every assertion below correctly.  Its sum is
-- -4 on the whole data, 6, 2 or -4 depending on the world.
INSERT INTO af_t VALUES (1,10),(1,20),(1,5),(2,7),(2,3),(3,4),(4,-6),(4,2);
SELECT add_provenance('af_t');
DO $$ BEGIN
  PERFORM set_prob(provenance(), 0.3) FROM af_t WHERE v = 10;
  PERFORM set_prob(provenance(), 0.5) FROM af_t WHERE v = 20;
  PERFORM set_prob(provenance(), 0.7) FROM af_t WHERE v = 5;
  PERFORM set_prob(provenance(), 0.4) FROM af_t WHERE v = 7;
  PERFORM set_prob(provenance(), 0.6) FROM af_t WHERE v = 3;
  PERFORM set_prob(provenance(), 0.8) FROM af_t WHERE v = 4;
  PERFORM set_prob(provenance(), 0.5) FROM af_t WHERE v = -6;
  PERFORM set_prob(provenance(), 0.5) FROM af_t WHERE v = 2;
END $$;

-- The value shown is the one plain SQL computes; its expectation is over the
-- worlds.  g=1 reads avg 11.666667 on the data as it is, and 10.368715 in
-- expectation once it is read per world -- floor of an average is not the
-- average of a floor.
CREATE TABLE af_r AS
  SELECT g, floor(avg(v)) AS f, ceil(avg(v)) AS c, abs(sum(v)) AS a,
         round(avg(v)) AS r, round(avg(v), 1) AS r1,
         round(expected(floor(avg(v)))::numeric, 6) AS e_f,
         round(expected(ceil(avg(v)))::numeric, 6) AS e_c,
         round(expected(abs(sum(v)))::numeric, 6) AS e_a,
         round(expected(round(avg(v)))::numeric, 6) AS e_r,
         round(expected(round(avg(v), 1))::numeric, 6) AS e_r1
  FROM af_t GROUP BY g;
SELECT remove_provenance('af_r');
SELECT * FROM af_r ORDER BY g;
DROP TABLE af_r;

-- A comparison on such a value is read per world too, and the groups in which
-- it holds in no world are dropped, as they are for a comparison on a bare
-- aggregate: only g=1 can reach floor(avg) >= 11 (in the worlds where 20 is
-- present and 5 is not, 0.5) or abs(sum) > 20 (0.395).
CREATE TABLE af_h AS
  SELECT 'floor(avg(v)) >= 11' AS pred, g,
         round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM af_t GROUP BY g HAVING floor(avg(v)) >= 11
  UNION ALL
  SELECT 'abs(sum(v)) > 20', g,
         round(probability_evaluate(provenance())::numeric, 6)
  FROM af_t GROUP BY g HAVING abs(sum(v)) > 20
  UNION ALL
  -- 0.245 for g=1 is the single world where only the 5 is present.
  SELECT 'round(avg(v), 1) <= 5', g,
         round(probability_evaluate(provenance())::numeric, 6)
  FROM af_t GROUP BY g HAVING round(avg(v), 1) <= 5;
SELECT remove_provenance('af_h');
SELECT * FROM af_h ORDER BY pred, g;
DROP TABLE af_h;

-- A rounding to a negative number of digits moves a value by more than one,
-- so the possibility condition declines it and every group is kept, with the
-- comparison still read per world: round(sum(v), -1) = 40 holds for g=1 in the
-- one world whose sum rounds to 40, the sum of 35 that needs all three rows
-- (0.3*0.5*0.7 = 0.105), and never for the others, which are kept as rows of
-- probability 0 because the condition declined.
CREATE TABLE af_n AS
  SELECT g, round(probability_evaluate(provenance())::numeric, 6) AS p
  FROM af_t GROUP BY g HAVING round(sum(v), -1) = 40;
SELECT remove_provenance('af_n');
SELECT * FROM af_n ORDER BY g;
DROP TABLE af_n;

SELECT remove_provenance('af_t');
DROP TABLE af_t;
