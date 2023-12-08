\set ECHO none
SET search_path TO provsql_test,provsql;

CREATE TABLE banzhaf_result AS
  SELECT name, city, banzhaf(c.provenance,p.provenance) FROM (
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

SELECT remove_provenance('banzhaf_result');

SELECT name, city, ROUND(banzhaf::numeric,3) AS banzhaf FROM banzhaf_result
ORDER BY city, name;

DROP TABLE banzhaf_result;

-- banzhaf computation in the non-probabilistic case
DO $$ BEGIN
  PERFORM set_prob(provenance(), 1.) FROM personnel;
END $$;

CREATE TABLE banzhaf_result AS
  SELECT name, city, banzhaf(c.provenance,p.provenance) FROM (
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

SELECT remove_provenance('banzhaf_result');

SELECT name, city, ROUND(banzhaf::numeric,3) AS banzhaf FROM banzhaf_result
ORDER BY city, name;

DROP TABLE banzhaf_result;

-- Put back original probability values
DO $$ BEGIN
  PERFORM set_prob(provenance(), id*1./10) FROM personnel;
END $$;

CREATE TABLE banzhaf_result1 AS
    SELECT city, provenance() FROM (
       (SELECT DISTINCT city FROM personnel)
     EXCEPT
       (SELECT p1.city
       FROM personnel p1, personnel p2
       WHERE p1.city = p2.city AND p1.id < p2.id
       GROUP BY p1.city
       ORDER BY p1.city)
       ) t;
SELECT remove_provenance('banzhaf_result1');
CREATE TABLE banzhaf_result2 AS
  SELECT * FROM banzhaf_result1, banzhaf_all_vars(provenance);

SELECT city, ROUND(value::numeric,3) AS banzhaf FROM banzhaf_result2
ORDER BY city, banzhaf;

DROP TABLE banzhaf_result1;
DROP TABLE banzhaf_result2;
