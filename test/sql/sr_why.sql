\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_why AS SELECT
  p1.city,
  sr_why(provenance(), 'personnel_name') AS why
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_why');
SELECT city, why
FROM result_why;

DROP TABLE result_why;
