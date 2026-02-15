\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_count AS SELECT
  city,
  COUNT(*) AS c
FROM personnel
GROUP BY city;

CREATE TABLE result_count2 AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM result_count
WHERE c > 2
ORDER BY city;

SELECT remove_provenance('result_count2');
SELECT city, formula, counting
FROM result_count2;

DROP TABLE result_count2;
DROP TABLE result_count;

CREATE TABLE result_sum AS SELECT
  city,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting FROM (
    SELECT city, SUM(id) AS s FROM personnel GROUP BY city
  ) t
WHERE s > 2 AND city<>'Berlin'
ORDER BY city;

SELECT remove_provenance('result_sum');
SELECT city, formula, counting
FROM result_sum;

DROP TABLE result_sum;
