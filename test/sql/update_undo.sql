\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE undo_test(id INT PRIMARY KEY);

INSERT INTO undo_test (id) VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('undo_test');

DELETE FROM query_provenance;

-- Test 1: undo a delete query
DELETE FROM undo_test WHERE id = 5;

SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 5;

CREATE TABLE undo_result AS SELECT undo(provsql) FROM query_provenance WHERE query = 'DELETE FROM undo_test WHERE id = 5;';
DROP TABLE undo_result;

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 5;

-- Test 2: undo an insert query
INSERT INTO undo_test (id) VALUES (6);

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 6;

CREATE TABLE undo_result AS SELECT undo(provsql) FROM query_provenance WHERE query = 'INSERT INTO undo_test (id) VALUES (6);';
DROP TABLE undo_result;

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 6;

-- Test 3: undo an update query
UPDATE undo_test SET id = 7 WHERE id = 1;

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 1;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 7;

CREATE TABLE undo_result AS SELECT undo(provsql) FROM query_provenance WHERE query = 'UPDATE undo_test SET id = 7 WHERE id = 1;';
DROP TABLE undo_result;

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 1;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 7;

-- Test 4: undo an undo query
CREATE TABLE undo_result AS SELECT undo(provsql) FROM query_provenance WHERE query = 'CREATE TABLE undo_result AS SELECT undo(provsql) FROM query_provenance WHERE query = ''DELETE FROM undo_test WHERE id = 5;'';';
DROP TABLE undo_result;

DROP TABLE undo_test_id;
SELECT create_provenance_mapping('undo_test_id', 'undo_test', 'id');
SELECT COUNT(*) FROM undo_test_id;
SELECT probability_evaluate(provenance) FROM undo_test_id WHERE value = 5;

-- Test 5: insert token tracking/ logging into query_provenance table
CREATE TABLE query_provenance_result AS
SELECT query FROM query_provenance ORDER BY ts;
SELECT remove_provenance('query_provenance_result');
SELECT * FROM query_provenance_result;
DROP TABLE query_provenance_result;

DELETE FROM query_provenance;

DROP TABLE undo_test;
DROP TABLE undo_test_id;