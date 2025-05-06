\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SELECT time, person, COUNT(room)
FROM s
GROUP BY time, person
HAVING COUNT(room) > 1 AND COUNT(room) < 5;
