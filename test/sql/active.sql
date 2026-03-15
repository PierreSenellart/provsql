\set ECHO none
\pset format unaligned
SET search_path TO provsql_test;

SET provsql.active=0;

SELECT DISTINCT p1.city
FROM personnel p1, personnel p2
WHERE p1.city = p2.city AND p1.id < p2.id
ORDER BY city;

  SELECT city FROM personnel
EXCEPT
  SELECT p1.city
  FROM personnel p1, personnel p2
  WHERE p1.city = p2.city AND p1.id < p2.id
  GROUP BY p1.city
ORDER BY city;
