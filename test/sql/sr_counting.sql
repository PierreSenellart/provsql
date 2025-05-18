\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_count', 'personnel', '1');
CREATE TABLE result_counting AS SELECT
  p1.city,
  sr_counting(provenance(),'personnel_count') AS counting
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_counting');
SELECT * FROM result_counting;

DROP TABLE result_counting;
