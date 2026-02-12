\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_having_on_count AS SELECT
  city,
  COUNT(*),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING count(*) > 2;

SELECT remove_provenance('result_having_on_count');
SELECT city, formula, counting
FROM result_having_on_count;

DROP TABLE result_having_on_count;

CREATE TABLE result_having_on_sum AS SELECT
  city,
  SUM(id),
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting
FROM personnel
GROUP BY city
HAVING SUM(id) > 2;

SELECT remove_provenance('result_having_on_sum');
SELECT city, formula, counting
FROM result_having_on_sum;

DROP TABLE result_having_on_sum;

CREATE TABLE result_having_why_count_leq3 AS SELECT
  city,
  COUNT(*),
  sr_why(provenance(), 'personnel_name') AS formula
FROM personnel
GROUP BY city
HAVING count(*) <= 3;

SELECT remove_provenance('result_having_why_count_leq3');
SELECT city, formula
FROM result_having_why_count_leq3;

DROP TABLE result_having_why_count_leq3;

CREATE TABLE result_having_why_sum_geq7 AS SELECT
  city,
  SUM(id),
  sr_why(provenance(), 'personnel_name') AS formula
FROM personnel
GROUP BY city
HAVING SUM(id) >= 7;

SELECT remove_provenance('result_having_why_sum_geq7');
SELECT city, formula
FROM result_having_why_sum_geq7;

DROP TABLE result_having_why_sum_geq7;

CREATE TABLE result_nested_having_formula AS
SELECT 1, sr_formula(provenance(), 'personnel_name') AS formula
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_formula');
SELECT formula
FROM result_nested_having_formula;

DROP TABLE result_nested_having_formula;

CREATE TABLE result_nested_having_why AS
SELECT 1, sr_why(provenance(), 'personnel_name') AS formula
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_why');
SELECT formula
FROM result_nested_having_why;

DROP TABLE result_nested_having_why;

CREATE TABLE result_nested_having_count AS
SELECT 1, sr_counting(provenance(), 'personnel_count') AS counting
FROM (
  SELECT city, count(*)
  FROM personnel
  GROUP BY city
  HAVING count(*) < 3
) AS tmp
GROUP BY 1;

SELECT remove_provenance('result_nested_having_count');
SELECT counting
FROM result_nested_having_count;

DROP TABLE result_nested_having_count;
