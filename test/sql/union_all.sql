\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE union_all_result AS
SELECT *,formula(provenance(),'personal_name') FROM (
  SELECT classification FROM personal WHERE city='Paris'
  UNION ALL
  SELECT value FROM personal_level
) t;

SELECT remove_provenance('union_all_result');
SELECT * FROM union_all_result ORDER BY classification;
DROP TABLE union_all_result;
