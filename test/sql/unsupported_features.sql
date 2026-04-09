\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

SELECT
    provenance ();

-- IN subquery
SELECT
    *
FROM
    personnel
WHERE
    city IN (
        SELECT
            city
        FROM
            personnel);

-- EXISTS subquery
SELECT
    *
FROM
    personnel p
WHERE
    EXISTS (
        SELECT 1
        FROM personnel q
        WHERE q.city = p.city AND q.id <> p.id);

-- NOT IN subquery
SELECT
    *
FROM
    personnel
WHERE
    name NOT IN (
        SELECT
            name
        FROM
            personnel
        WHERE
            city = 'Paris');

-- Scalar subquery in SELECT
SELECT
    name,
    (SELECT COUNT(*) FROM personnel) AS total
FROM
    personnel;

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
