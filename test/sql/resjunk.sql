\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE resjunk AS
SELECT
    city,
    provenance ()
FROM (
    SELECT
        p1.city
    FROM
        personnel p1
        JOIN personnel p2 ON p1.city = p2.city
    GROUP BY
        p1.city,
        p2.name,
        p2.id) t;

SELECT
    remove_provenance ('resjunk');

ALTER TABLE resjunk
    DROP COLUMN provenance;

SELECT
    *
FROM
    resjunk
ORDER BY
    city;

DROP TABLE resjunk;
