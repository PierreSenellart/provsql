\set ECHO none

SELECT * FROM (VALUES (1)) t, personal;

WITH Q(a) AS (SELECT 1) SELECT * FROM Q, personal;

SELECT COUNT(*) FROM personal;

SELECT * FROM personal WHERE city IN (SELECT city FROM personal);

SELECT DISTINCT ON (city) * FROM personal;

SELECT * FROM personal INTERSECT SELECT * FROM personal;
