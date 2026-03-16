\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  ROUND(probability_evaluate(provenance())::numeric, 2) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*) >=1 AND COUNT(*) <= 2;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT * FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;
CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  ROUND(probability_evaluate(provenance())::numeric, 2) AS prob
FROM personnel
GROUP BY city
HAVING NOT(COUNT(*) = 0 OR COUNT(*) > 2);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT * FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;
CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  COUNT(*) AS c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*) > 2 OR COUNT(*) = 1;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS SELECT
  city,
  COUNT(*) AS c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING NOT(NOT(COUNT(*) > 2) AND COUNT(*) <> 1);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;

DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c FROM personnel GROUP BY city
  ) AS t
WHERE c>2 OR c=1;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  s,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c, SUM(id) AS s FROM personnel GROUP by city
  ) t
  WHERE c=2 AND s>4;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, s, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT city, COUNT(*) AS c, SUM(id) AS s, probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*)=2 AND SUM(id)>4;

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, s, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors
ORDER BY city;
DROP TABLE result_having_boolean_connectors;

CREATE TABLE result_having_boolean_connectors AS
SELECT
  city,
  c,
  sr_formula(provenance(), 'personnel_name') AS formula,
  sr_counting(provenance(), 'personnel_count') AS counting,
  probability_evaluate(provenance()) AS prob FROM (
    SELECT city, COUNT(*) AS c FROM personnel GROUP BY city
  ) AS t
WHERE city='Paris' AND (c>2 OR c=1);

SELECT remove_provenance('result_having_boolean_connectors');
SELECT city, c, formula, counting, ROUND(prob::NUMERIC, 2)
FROM result_having_boolean_connectors;
DROP TABLE result_having_boolean_connectors;
