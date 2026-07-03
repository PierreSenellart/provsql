\set ECHO none
\set SHOW_CONTEXT never
\pset format unaligned

CREATE TABLE expected_result AS
SELECT city, expected(COUNT(*)) AS c1, expected(COUNT(id)) AS c2, expected(SUM(id)) AS s, expected(MIN(id)) AS min, expected(MAX(id)) as max
FROM personnel
GROUP BY CITY;

SELECT remove_provenance('expected_result');

SELECT city, ROUND(c1::numeric,2) AS c1, ROUND(c2::numeric,2) AS c2, ROUND(s::numeric,2) AS s, ROUND(min::numeric,2) AS min, ROUND(max::numeric,2) AS max
FROM expected_result
ORDER BY city;

DROP TABLE expected_result;

CREATE TABLE expected_result AS
SELECT city, expected(COUNT(*),provenance()) AS c1, expected(COUNT(id),provenance()) AS c2, expected(SUM(id),provenance()) AS s, expected(MIN(id),provenance()) AS min, expected(MAX(id),provenance()) as max
FROM personnel
GROUP BY CITY;

SELECT remove_provenance('expected_result');

SELECT city, ROUND(c1::numeric,2) AS c1, ROUND(c2::numeric,2) AS c2, ROUND(s::numeric,2) AS s, ROUND(min::numeric,2) AS min, ROUND(max::numeric,2) AS max
FROM expected_result
ORDER BY city;

DROP TABLE expected_result;

-- Non-supported
SELECT expected('toto'::text) FROM personnel;

-- AVG moments are supported: the exact independent-rows arm computes
-- E[AVG | at least one row present] from the joint (sum, count)
-- distribution, with no sampling.  Materialise + remove_provenance to
-- keep the run-dependent group token out of the output.
CREATE TABLE expected_result AS SELECT expected(AVG(id)) AS a FROM personnel;
SELECT remove_provenance('expected_result');
SELECT ROUND(a::numeric, 6) AS avg_expected FROM expected_result;
DROP TABLE expected_result;
