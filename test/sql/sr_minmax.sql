\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

/* The min-max m-semiring: ⊕ = enum-min, ⊗ = enum-max.
   Security shape: alternative derivations combine to the least sensitive
   label, joins combine to the most sensitive label. The carrier is the
   classification_level enum from add_provenance.sql. */
SELECT create_provenance_mapping('personnel_level', 'personnel', 'classification');

CREATE TABLE result_minmax AS SELECT
  p1.city,
  sr_minmax(provenance(),'personnel_level','unclassified'::classification_level) AS clearance
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_minmax');
SELECT * FROM result_minmax;

DROP TABLE result_minmax;
