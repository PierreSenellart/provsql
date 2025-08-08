\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE distinct_result AS
  SELECT *, sr_formula(provenance(),'personnel_name') AS formula
  FROM (
    SELECT DISTINCT classification FROM personnel
  ) t;

SELECT remove_provenance('distinct_result');
SELECT classification,replace(formula,'(Paul ⊕ Nancy)','(Nancy ⊕ Paul)') AS formula FROM distinct_result ORDER BY classification;
DROP TABLE distinct_result;

CREATE TABLE distinct_result AS
  SELECT COUNT(DISTINCT name)
  FROM personnel
  GROUP BY city;
SELECT remove_provenance('distinct_result');
SELECT * FROM distinct_result ORDER BY count::text;
DROP TABLE distinct_result;
