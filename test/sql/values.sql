\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

CREATE TABLE tvalues AS
SELECT
    *,
    formula (provenance (), 'personnel_name')
FROM (
    VALUES (1)) t,
    personnel;

SELECT
    remove_provenance ('tvalues');

SELECT
    *
FROM
    tvalues
ORDER BY
    id;
