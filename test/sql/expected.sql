\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE expected_result AS
SELECT city, expected(COUNT(*)) AS c1, expected(COUNT(id)) AS c2, expected(SUM(id)) AS s
FROM personnel
GROUP BY CITY;

SELECT remove_provenance('expected_result');

SELECT city, ROUND(c1::numeric,2) AS c1, ROUND(c2::numeric,2) AS c2, ROUND(s::numeric,2) AS s
FROM expected_result
ORDER BY city;

DROP TABLE expected_result;

-- Non-supported
SELECT expected('toto'::text) FROM personnel;
SELECT expected(MAX(id)) FROM personnel;
SELECT expected(AVG(id)) FROM personnel;
