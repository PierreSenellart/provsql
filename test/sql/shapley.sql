\set ECHO none
\pset format unaligned

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

-- Shapley computation in the non-probabilistic case.  A probability is
-- written once, so a different assignment means a different input gate
-- per row: replace_input mints it and the row's provsql column carries
-- it.  The table stays TID, and personnel_name follows the tokens.
UPDATE personnel SET provsql = provsql.replace_input(provsql, 1.);

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

-- Put back the original probability values, the same way.
UPDATE personnel SET provsql = provsql.replace_input(provsql, id*1./10);

CREATE TABLE shapley_result1 AS
    SELECT city, provenance() FROM (
       (SELECT DISTINCT city FROM personnel)
     EXCEPT
       (SELECT p1.city
       FROM personnel p1, personnel p2
       WHERE p1.city = p2.city AND p1.id < p2.id
       GROUP BY p1.city
       ORDER BY p1.city)
       ) t;
SELECT remove_provenance('shapley_result1');
CREATE TABLE shapley_result2 AS
  SELECT * FROM shapley_result1, shapley_all_vars(provenance);

SELECT city, ROUND(value::numeric,3) AS shapley FROM shapley_result2
ORDER BY city, shapley;

DROP TABLE shapley_result1;
DROP TABLE shapley_result2;
