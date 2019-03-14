\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE a(f int);
CREATE TABLE b(f int);
INSERT INTO a VALUES(42);
INSERT INTO b VALUES(42);

SELECT add_provenance('a');
SELECT add_provenance('b');

CREATE TABLE c as SELECT * FROM (SELECT * FROM a UNION SELECT * FROM b) t;

SELECT remove_provenance('c');

SELECT * FROM c;

DROP TABLE a;
DROP TABLE b;
DROP TABLE c;
