\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_formula AS SELECT
  *, sr_formula(provenance(),'personnel_name') AS formula FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT p1.city
     FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city)
  ) t
ORDER BY city;

SELECT remove_provenance('result_formula');
SELECT city, replace(formula,'(Dave ⊗ Nancy) ⊕ (Dave ⊗ Magdalen)','(Dave ⊗ Magdalen) ⊕ (Dave ⊗ Nancy)') AS formula FROM result_formula;

DROP TABLE result_formula;

CREATE TABLE result_formula AS SELECT
  *, sr_formula(provenance(),'personnel_name') AS formula FROM (
    (SELECT city
     FROM personnel
     WHERE FALSE)
  EXCEPT
    (SELECT city
     FROM personnel p)
  ) t
ORDER BY city;

SELECT remove_provenance('result_formula');
SELECT * FROM result_formula;

DROP TABLE result_formula;
