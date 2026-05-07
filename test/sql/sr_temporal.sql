\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET TIME ZONE 'UTC';
SET datestyle = 'iso';

SELECT create_provenance_mapping('personnel_validity', 'personnel',
  $$ CASE id
       WHEN 1 THEN '{[2020-01-01,2021-01-01)}'::tstzmultirange
       WHEN 2 THEN '{[2021-01-01,2022-01-01)}'::tstzmultirange
       WHEN 3 THEN '{[2020-01-01,2022-01-01)}'::tstzmultirange
       WHEN 4 THEN '{[2022-01-01,2023-01-01)}'::tstzmultirange
       WHEN 5 THEN '{[2020-01-01,2021-01-01)}'::tstzmultirange
       WHEN 6 THEN '{[2021-01-01,2022-01-01)}'::tstzmultirange
       WHEN 7 THEN '{[2022-01-01,2023-01-01)}'::tstzmultirange
     END $$);

CREATE TABLE result_temporal AS SELECT
  p1.city,
  sr_temporal(provenance(),'personnel_validity') AS validity
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_temporal');
SELECT * FROM result_temporal;

DROP TABLE result_temporal;
