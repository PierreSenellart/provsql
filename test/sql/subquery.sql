\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE subquery_result AS
SELECT name AS name1, name2
FROM (
  SELECT p1.*, p2.name AS name2
  FROM personnel p1, personnel p2
  WHERE p1.city=p2.city AND p1.name<>p2.name
) t;

SELECT remove_provenance('subquery_result');

SELECT * FROM subquery_result
ORDER BY name1, name2;
