\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_cost', 'personnel', 'id::float');
CREATE TABLE result_tropical AS SELECT
  p1.city,
  sr_tropical(provenance(),'personnel_cost') AS cost
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_tropical');
SELECT * FROM result_tropical;

DROP TABLE result_tropical;
