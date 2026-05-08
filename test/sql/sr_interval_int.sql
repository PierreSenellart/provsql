\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_pages', 'personnel',
  $$ CASE id
       WHEN 1 THEN '{[1,10)}'::int4multirange
       WHEN 2 THEN '{[10,20)}'::int4multirange
       WHEN 3 THEN '{[1,30)}'::int4multirange
       WHEN 4 THEN '{[20,30)}'::int4multirange
       WHEN 5 THEN '{[5,25)}'::int4multirange
       WHEN 6 THEN '{[10,30)}'::int4multirange
       WHEN 7 THEN '{[15,35)}'::int4multirange
     END $$);

CREATE TABLE result_interval_int AS SELECT
  p1.city,
  sr_interval_int(provenance(),'personnel_pages') AS pages
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_interval_int');
SELECT * FROM result_interval_int;

DROP TABLE result_interval_int;
