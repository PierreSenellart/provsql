\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_having_on_count AS SELECT
  city,
  COUNT(*),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(),'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING count(*) > 2 ;

SELECT remove_provenance('result_having_on_count');
SELECT city, formula, counting
FROM result_having_on_count;

DROP TABLE result_having_on_count;

CREATE TABLE result_having_on_sum AS SELECT
  city,
  SUM(id),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(),'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING SUM(id)>2;

SELECT remove_provenance('result_having_on_sum');
SELECT city, formula, counting
FROM result_having_on_sum;

DROP TABLE result_having_on_sum;
