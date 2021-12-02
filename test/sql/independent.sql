\set ECHO none
SET search_path TO provsql_test,provsql;

-- Will fail because this is not independent
CREATE TABLE independent_result AS
SELECT city, probability_evaluate(provenance(),'independent') AS prob
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

CREATE TABLE independent_result AS
SELECT city, probability_evaluate(provenance(),'independent') AS prob
FROM personnel
GROUP BY city
ORDER BY city;

SELECT remove_provenance('independent_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM independent_result;
DROP TABLE independent_result;
