\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

/* Example of provenance evaluation */
SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
CREATE TABLE result_formula AS SELECT
  p1.city,
  sr_formula(provenance(), 'personnel_name') AS formula
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_formula');
SELECT
  city,
  replace(formula,'(Dave ⊗ Nancy) ⊕ (Dave ⊗ Magdalen)','(Dave ⊗ Magdalen) ⊕ (Dave ⊗ Nancy)') AS formula
FROM result_formula;

DROP TABLE result_formula;
