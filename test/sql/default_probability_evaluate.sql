\set ECHO none
SET search_path TO provsql_test,provsql;

CREATE TABLE default_result AS
SELECT city, probability_evaluate(provenance()) AS prob
FROM (
  SELECT DISTINCT city
  FROM personnel
EXCEPT 
  SELECT p1.city
  FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t
ORDER BY CITY;

SELECT remove_provenance('default_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM default_result;
DROP TABLE default_result;
