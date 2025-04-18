\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE personnel_active AS
SELECT (value<>'Nancy' AND value<>'Magdalen') AS value, provenance
FROM personnel_name;

CREATE TABLE boolean_result AS
  SELECT p1.city, sr_boolean(provenance(), 'personnel_active') AS b
  FROM personnel p1 JOIN personnel p2 ON p1.city=p2.city AND p1.id<p2.id
  GROUP BY p1.city;
SELECT remove_provenance('boolean_result');
SELECT city FROM boolean_result WHERE b;
DROP TABLE boolean_result;
