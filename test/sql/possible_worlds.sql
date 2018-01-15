\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE pw_result AS
SELECT city, probability_evaluate(provenance(),'p','possible-worlds') AS prob
FROM (
  SELECT DISTINCT city
  FROM personnel
EXCEPT 
  SELECT p1.city
  FROM personnel p1,personnel p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t
ORDER BY city;

SELECT remove_provenance('pw_result');

SELECT * FROM pw_result;
DROP TABLE pw_result;
