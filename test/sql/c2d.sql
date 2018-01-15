\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE c2d_result AS
SELECT city, probability_evaluate(provenance(),'p','compilation','c2d') AS prob
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

SELECT remove_provenance('c2d_result');

SELECT * FROM c2d_result;
DROP TABLE c2d_result;
