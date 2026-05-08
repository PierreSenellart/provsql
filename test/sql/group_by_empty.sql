\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE group_by_empty_result AS
  SELECT provenance()
  FROM personnel
  GROUP BY ();

SELECT remove_provenance('group_by_empty_result');
SELECT sr_minmax(provenance,'personnel_level','unclassified'::classification_level) FROM group_by_empty_result;
