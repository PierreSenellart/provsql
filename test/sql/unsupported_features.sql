\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

SELECT
    provenance ();

WITH Q (
    a
) AS (
    SELECT
        1
)
SELECT
    *
FROM
    Q,
    personnel;

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
