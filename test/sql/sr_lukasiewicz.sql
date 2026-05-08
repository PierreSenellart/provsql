\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_evidence', 'personnel', '(1.0-0.1*id)::float');
CREATE TABLE result_lukasiewicz AS SELECT
  p1.city,
  round(sr_lukasiewicz(provenance(),'personnel_evidence')::numeric, 6) AS evidence
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_lukasiewicz');
SELECT * FROM result_lukasiewicz;

DROP TABLE result_lukasiewicz;
