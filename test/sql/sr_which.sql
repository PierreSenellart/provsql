\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_which AS SELECT
  p1.city,
  sr_which(provenance(), 'personnel_name') AS which
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_which');
SELECT city, which
FROM result_which;

DROP TABLE result_which;
