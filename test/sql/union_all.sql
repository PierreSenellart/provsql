\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE union_all_result AS
SELECT *,formula(provenance(),'personnel_name') FROM (
  SELECT classification FROM personnel WHERE city='Paris'
  UNION ALL
  SELECT classification FROM personnel
) t;

SELECT remove_provenance('union_all_result');
SELECT * FROM union_all_result ORDER BY classification;
DROP TABLE union_all_result;
