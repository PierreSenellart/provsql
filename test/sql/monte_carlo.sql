\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE mc_result AS
SELECT city, probability_evaluate(provenance(),'p','monte-carlo','10000') AS prob
FROM (
  SELECT DISTINCT city
  FROM personnel
EXCEPT 
  SELECT p1.city
  FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;

SELECT remove_provenance('mc_result');

SELECT city, ROUND(prob::numeric,1) FROM mc_result WHERE city = 'Paris';
DROP TABLE mc_result;
