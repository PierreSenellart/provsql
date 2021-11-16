\set ECHO none
SET search_path TO provsql_test,provsql;

CREATE TABLE treedec_result AS
SELECT city, probability_evaluate(provenance(),'tree-decomposition') AS prob
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

SELECT remove_provenance('treedec_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM treedec_result;
DROP TABLE treedec_result;
