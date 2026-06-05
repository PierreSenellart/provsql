\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

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

SELECT
    *
FROM
    personnel
EXCEPT
SELECT
    *
FROM
    personnel
EXCEPT
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

-- Hand-made provsql column inside a set-operation arm
SELECT name, provenance() AS provsql FROM personnel
EXCEPT ALL
SELECT name, provenance() AS provsql FROM personnel;
