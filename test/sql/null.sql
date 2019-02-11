\set ECHO none
SET search_path TO public, provsql;

CREATE TABLE t(x INT);
INSERT INTO t VALUES(NULL);
SELECT add_provenance('t');
CREATE TABLE t2 AS SELECT * FROM t;
SELECT remove_provenance('t2');
SELECT * FROM t2;
DROP TABLE t;
DROP TABLE t2;
