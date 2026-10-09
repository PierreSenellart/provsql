\set ECHO none
\pset format unaligned

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

/* The enum argument is read for its type only: NULL::type gives the same
   result as a value of the enum.  A NULL token or mapping gives NULL. */
CREATE TABLE result_minmax_null AS SELECT
  p1.city,
  sr_minmax(provenance(),'personnel_level',NULL::classification_level) AS clearance
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city;
SELECT remove_provenance('result_minmax_null');
SELECT bool_and(r.clearance = n.clearance) AND count(*) = 3 AS null_sample_same
FROM result_minmax r JOIN result_minmax_null n USING (city);
DROP TABLE result_minmax_null;
SELECT sr_minmax(NULL,'personnel_level',NULL::classification_level) IS NULL AS null_token,
       sr_maxmin(NULL,'personnel_level',NULL::classification_level) IS NULL AS null_token_maxmin,
       sr_minmax((SELECT provsql FROM personnel LIMIT 1),NULL,NULL::classification_level) IS NULL
         AS null_mapping;

DROP TABLE result_minmax;
