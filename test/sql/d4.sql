\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE d4_result AS
SELECT city, probability_evaluate(provenance(),'compilation','d4') AS prob
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

SELECT remove_provenance('d4_result');

SELECT city, ROUND(prob::numeric,2) AS prob FROM d4_result;
DROP TABLE d4_result;

CREATE TABLE d4_result AS
  SELECT name, city, shapley(c.provenance,p.provenance,'compilation','d4') FROM (
    SELECT provenance() from (SELECT DISTINCT 1 FROM (
      (SELECT DISTINCT city FROM personnel)
    EXCEPT
      (SELECT p1.city
      FROM personnel p1, personnel p2
      WHERE p1.city = p2.city AND p1.id < p2.id
      GROUP BY p1.city
      ORDER BY p1.city)
      ) t
    ) u)
  AS c,
  (SELECT *, provenance() FROM personnel) AS p;

SELECT remove_provenance('d4_result');

SELECT name, city, ROUND(shapley::numeric,3) AS shapley FROM d4_result
ORDER BY city, name;

DROP TABLE d4_result;
