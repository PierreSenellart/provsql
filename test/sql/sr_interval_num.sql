\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT create_provenance_mapping('personnel_validity_num', 'personnel',
  $$ CASE id
       WHEN 1 THEN '{[0.0,1.0]}'::nummultirange
       WHEN 2 THEN '{[2.0,3.0]}'::nummultirange
       WHEN 3 THEN '{[1.0,5.0]}'::nummultirange
       WHEN 4 THEN '{[3.0,5.0]}'::nummultirange
       WHEN 5 THEN '{[2.0,4.0]}'::nummultirange
       WHEN 6 THEN '{[3.5,4.5]}'::nummultirange
       WHEN 7 THEN '{[4.0,5.0]}'::nummultirange
     END $$);

CREATE TABLE result_interval_num AS SELECT
  p1.city,
  sr_interval_num(provenance(),'personnel_validity_num') AS validity
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
GROUP BY p1.city
ORDER BY p1.city;

SELECT remove_provenance('result_interval_num');
SELECT * FROM result_interval_num;

DROP TABLE result_interval_num;
