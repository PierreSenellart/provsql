\set ECHO none
SET search_path TO public, provsql;

SELECT provenance();

SELECT * FROM (VALUES (1)) t, personnel;

WITH Q(a) AS (SELECT 1) SELECT * FROM Q, personnel;

SELECT COUNT(*) FROM personnel;

SELECT * FROM personnel WHERE city IN (SELECT city FROM personnel);

SELECT DISTINCT ON (city) * FROM personnel;

SELECT DISTINCT 1 FROM personnel GROUP BY city;

SELECT * FROM personnel INTERSECT SELECT * FROM personnel;
