\set ECHO none
SET search_path TO provsql_test, provsql;

CREATE TABLE x (n integer);
INSERT INTO X VALUES (10),(20);
SELECT add_provenance('x');

CREATE TABLE u AS (SELECT * FROM x);

SELECT attname, typname
FROM pg_attribute JOIN pg_type ON atttypid=oid
WHERE attrelid ='u'::regclass AND attnum>0
ORDER BY attname;

DROP TABLE x;
DROP TABLE u;
