\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_choose AS SELECT
  city, sr_formula(choose(position), 'personnel_name') AS formula
FROM personnel
GROUP BY city;

SELECT remove_provenance('result_choose');
SELECT city, formula FROM result_choose ORDER BY city;

DROP TABLE result_choose;
