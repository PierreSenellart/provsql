\set ECHO none
\pset format unaligned

SELECT
    provenance ();

SELECT DISTINCT ON (city)
    *
FROM
    personnel;

SELECT DISTINCT
    1
FROM
    personnel
GROUP BY
    city;

SELECT
    *
FROM
    personnel
INTERSECT
SELECT
    *
FROM
    personnel;

-- DISTINCT on aggregate results
SELECT DISTINCT city, COUNT(*) FROM personnel GROUP BY city;

-- UNION (non-ALL) on aggregate results
SELECT city, COUNT(*) FROM personnel GROUP BY city
UNION
SELECT city, COUNT(*) FROM personnel WHERE city='Paris' GROUP BY city;

-- ORDER BY on aggregate from subquery
SELECT city, cnt FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t ORDER BY cnt;

-- GROUP BY on aggregate from subquery
SELECT cnt, COUNT(*) FROM (SELECT city, COUNT(*) AS cnt FROM personnel GROUP BY city) t GROUP BY cnt;

-- Hand-made provsql column (collides with the auto-added provenance column)
SELECT name, provenance() AS provsql FROM personnel;

-- Hand-made provsql column from a plain expression
SELECT name AS provsql FROM personnel;

-- Hand-made provsql column from another uuid column
SELECT u AS provsql FROM (SELECT gen_random_uuid() AS u, name FROM personnel) s;

-- Hand-made provsql column inside a set-operation arm
SELECT name, provenance() AS provsql FROM personnel
UNION ALL
SELECT name, provenance() AS provsql FROM personnel;

-- EXCEPT ALL over tracked relations, at the top level and nested
SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel;
SELECT name FROM (SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel) t;
WITH c AS (SELECT name FROM personnel EXCEPT ALL SELECT name FROM personnel) SELECT * FROM c;
