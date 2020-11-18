\set ECHO none
SET search_path TO public,provsql;

CREATE TABLE x (n integer);
INSERT INTO X VALUES (10),(20);
SELECT add_provenance('x');

CREATE TABLE u AS (SELECT * FROM x);

\d u

DROP TABLE x;
DROP TABLE u;
