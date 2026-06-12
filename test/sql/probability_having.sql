\set ECHO none
\pset format unaligned

CREATE TABLE probability_having_result AS
SELECT city, probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*)=1;

SELECT remove_provenance('probability_having_result');

SELECT city, ROUND(prob::numeric,2) AS prob
FROM probability_having_result
ORDER BY city;

DROP TABLE probability_having_result;

CREATE TABLE probability_having_result AS
SELECT city, probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*)>=2;

SELECT remove_provenance('probability_having_result');

SELECT city, ROUND(prob::numeric,2) AS prob
FROM probability_having_result
ORDER BY city;

DROP TABLE probability_having_result;

CREATE TABLE probability_having_result AS
SELECT city, probability_evaluate(provenance()) AS prob
FROM personnel
GROUP BY city
HAVING COUNT(*)>=3;

SELECT remove_provenance('probability_having_result');

SELECT city, ROUND(prob::numeric,2) AS prob
FROM probability_having_result
ORDER BY city;

DROP TABLE probability_having_result;
