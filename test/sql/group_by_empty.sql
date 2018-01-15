\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE group_by_empty_result AS
  SELECT provenance()
  FROM personnel
  GROUP BY ();

SELECT remove_provenance('group_by_empty_result');
SELECT security(provenance,'personnel_level') FROM group_by_empty_result;
