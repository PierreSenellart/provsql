\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE dsharp_result AS
SELECT city, probability_evaluate(provenance(),'p','compilation','dsharp') AS prob
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

SELECT remove_provenance('dsharp_result');

SELECT * FROM dsharp_result;
DROP TABLE dsharp_result;
