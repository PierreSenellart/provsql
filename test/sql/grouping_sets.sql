\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- GROUPING SETS, ROLLUP and CUBE: the UNION ALL of one GROUP BY per set, the
-- grouping columns not in a set NULL in its rows (not in the aggregates),
-- GROUPING() a constant; the empty set is an aggregation without GROUP BY,
-- one row, even over no row.
CREATE TABLE gs_r1 AS
  SELECT city, count(*) AS n,
         sr_counting(provenance(), 'personnel_count') AS counting
  FROM personnel GROUP BY GROUPING SETS ((), (city));
SELECT remove_provenance('gs_r1');
SELECT * FROM gs_r1 ORDER BY city NULLS FIRST;
DROP TABLE gs_r1;

CREATE TABLE gs_r2 AS
  SELECT city, classification, count(*) AS n,
         GROUPING(city, classification) AS g
  FROM personnel GROUP BY ROLLUP (city, classification)
  HAVING count(*) > 1;
SELECT remove_provenance('gs_r2');
SELECT * FROM gs_r2 ORDER BY g, city, classification;
DROP TABLE gs_r2;

CREATE TABLE gs_r3 AS
  SELECT upper(city) AS c, position, count(*) AS n
  FROM personnel GROUP BY CUBE (city, position);
SELECT remove_provenance('gs_r3');
SELECT count(*) AS rows, count(*) FILTER (WHERE c IS NULL AND position IS NULL)
  AS total FROM gs_r3;
DROP TABLE gs_r3;

CREATE TABLE gs_r4 AS
  SELECT count(*) AS n FROM personnel WHERE id > 100
  GROUP BY GROUPING SETS ((city), ());
SELECT remove_provenance('gs_r4');
SELECT * FROM gs_r4;
DROP TABLE gs_r4;

-- COUNT(DISTINCT) over the empty grouping set: one row, certain, even when
-- no row matches (count 0), as without DISTINCT.
CREATE TABLE gs_r5 AS
  SELECT city, count(DISTINCT classification) AS n,
         probability_evaluate(provenance()) AS p
  FROM personnel WHERE id > 100 GROUP BY ROLLUP (city);
SELECT remove_provenance('gs_r5');
SELECT city, n::text AS n, p FROM gs_r5;
DROP TABLE gs_r5;
CREATE TABLE gs_r6 AS
  SELECT count(DISTINCT classification) AS n, 7 AS k,
         probability_evaluate(provenance()) AS p
  FROM personnel;
SELECT remove_provenance('gs_r6');
SELECT n::text AS n, k, p FROM gs_r6;
DROP TABLE gs_r6;
