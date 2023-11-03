\set ECHO none
SET search_path TO provsql_test,provsql;

CREATE TABLE shapley_result AS
  SELECT name, city, shapley(c.provenance,p.provenance) FROM (
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

SELECT remove_provenance('shapley_result');

SELECT name, city, ROUND(shapley::numeric,3) AS shapley FROM shapley_result
ORDER BY city, name;

DROP TABLE shapley_result;
