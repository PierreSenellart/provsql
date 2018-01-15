\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE union_result AS
SELECT *,formula(provenance(),'personnel_name') FROM (
  SELECT classification FROM personnel WHERE city='Paris'
  UNION
  SELECT classification FROM personnel
) t;

SELECT remove_provenance('union_result');
SELECT * FROM union_result ORDER BY classification;
DROP TABLE union_result;
