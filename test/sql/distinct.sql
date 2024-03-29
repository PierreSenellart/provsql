\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE distinct_result AS
  SELECT *, formula(provenance(),'personnel_name')
  FROM (
    SELECT DISTINCT classification FROM personnel
  ) t;

SELECT remove_provenance('distinct_result');
SELECT * FROM distinct_result ORDER BY classification;
DROP TABLE distinct_result;
