\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE group_by_provenance_result AS
  SELECT city, formula(provenance(),'personnel_name')
  FROM personnel
  GROUP BY city, provenance();

SELECT remove_provenance('group_by_provenance_result');
SELECT * FROM group_by_provenance_result ORDER BY city, formula;
DROP TABLE group_by_provenance_result;
