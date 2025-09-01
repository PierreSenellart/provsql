\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- From https://github.com/PierreSenellart/provsql/issues/60

CREATE TABLE test (id INT PRIMARY KEY);
SELECT add_provenance('test');
SELECT create_provenance_mapping('pm', 'test', 'id');

INSERT INTO test (id) VALUES (1), (2), (3), (4), (5);

CREATE TABLE test2 as select * FROM test WHERE id = 6;
SELECT id, sr_formula(provenance(), 'pm') FROM test2;

CREATE TABLE test3 as select count(*) FROM test WHERE id = 6;
CREATE TABLE result_test AS
SELECT count, sr_formula(provenance(), 'pm') FROM test3;
SELECT remove_provenance('result_test');
SELECT * FROM result_test;

DROP TABLE test;
DROP TABLE test2;
DROP TABLE test3;
DROP TABLE pm;
DROP TABLE result_test;
