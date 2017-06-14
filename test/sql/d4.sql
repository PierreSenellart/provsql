\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE d4_result AS
SELECT city, probability_evaluate(provenance(),'p','compilation','d4') AS prob
FROM (
  SELECT DISTINCT city
  FROM personal
EXCEPT 
  SELECT p1.city
  FROM personal p1,personal p2
  WHERE p1.id<p2.id AND p1.city=p2.city
  GROUP BY p1.city
) t;

SELECT remove_provenance('d4_result');

SELECT * FROM d4_result;
DROP TABLE d4_result;
