\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_boolexpr AS SELECT
  p1.city,
  sr_boolexpr(provenance()) AS boolexpr
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;

SELECT remove_provenance('result_boolexpr');
SELECT
  city,
  regexp_replace(boolexpr, 'x[0-9]', 'x', 'g')
FROM result_boolexpr
ORDER BY city;

DROP TABLE result_boolexpr;
