\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_provxml AS SELECT
  *, to_provxml(provenance(),'personnel_name') AS provxml FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT p1.city
     FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city)
  ) t
ORDER BY city;

SELECT remove_provenance('result_provxml');
SELECT city, regexp_replace(provxml,'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}','UUID','g') AS provxml FROM result_provxml;

DROP TABLE result_provxml;

CREATE TABLE result_provxml AS SELECT
  *, to_provxml(provenance()) AS provxml FROM (
    (SELECT DISTINCT city FROM personnel)
  EXCEPT
    (SELECT p1.city
     FROM personnel p1, personnel p2
     WHERE p1.city = p2.city AND p1.id < p2.id
     GROUP BY p1.city)
  ) t
ORDER BY city;

SELECT remove_provenance('result_provxml');
SELECT city, regexp_replace(provxml,'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}','UUID','g') AS provxml FROM result_provxml;

DROP TABLE result_provxml;
