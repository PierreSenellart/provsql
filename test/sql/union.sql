\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE union_result AS
SELECT *,sr_formula(provenance(),'personnel_name') AS formula FROM (
  SELECT classification FROM personnel WHERE city='Paris'
  UNION
  SELECT classification FROM personnel
) t;

SELECT remove_provenance('union_result');
SELECT classification,replace(formula,'Paul ⊕ Nancy','Nancy ⊕ Paul') AS formula FROM union_result ORDER BY classification;
DROP TABLE union_result;

CREATE TABLE union_result AS
SELECT *,formula(provenance(),'personnel_name') AS formula FROM (
  SELECT FROM personnel WHERE city='Paris'
  UNION
  SELECT FROM personnel
) t;

SELECT remove_provenance('union_result');
SELECT * FROM union_result;
DROP TABLE union_result;
