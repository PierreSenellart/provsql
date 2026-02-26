\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING AVG(id) < 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING AVG(id) = 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING AVG(id) > 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING AVG(id) <> 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING MAX(id) < 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING MIN(id) < 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING MAX(id) >= 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;

CREATE TABLE result_having_other_operator AS SELECT
  city,
  probability_evaluate(provenance()) AS p
FROM personnel
GROUP BY city
HAVING MIN(id) >= 4;

SELECT remove_provenance('result_having_other_operator');
SELECT city, ROUND(p::NUMERIC, 2) FROM result_having_other_operator WHERE p>0 ORDER BY city;

DROP TABLE result_having_other_operator;
