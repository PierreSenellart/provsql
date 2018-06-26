\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE nested_union_result AS
SELECT *,formula(provenance(),'personnel_name') FROM (
  SELECT city FROM personnel WHERE classification != 'unclassified'
  UNION
  SELECT city FROM personnel WHERE position != 'Janitor'
  UNION
  SELECT city FROM personnel WHERE name LIKE '%n'
  UNION
  SELECT city FROM personnel WHERE position LIKE '%agent'
) t;

SELECT remove_provenance('nested_union_result');
SELECT * FROM nested_union_result ORDER BY city;
DROP TABLE nested_union_result;
