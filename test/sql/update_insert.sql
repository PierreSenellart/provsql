\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE insert_test(id INT PRIMARY KEY);

INSERT INTO insert_test (id)
VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('insert_test');

DELETE FROM query_provenance;

-- Test 1: single row insertion
SELECT create_provenance_mapping('insert_test_id', 'insert_test', 'id');
SELECT COUNT(*) FROM insert_test_id;
SELECT probability_evaluate(provenance) FROM insert_test_id WHERE value = 1;

DROP TABLE insert_test_id;
INSERT INTO insert_test VALUES (6);
SELECT create_provenance_mapping('insert_test_id', 'insert_test', 'id');
SELECT COUNT(*) FROM insert_test_id;
SELECT probability_evaluate(provenance) FROM insert_test_id WHERE value = 6;

-- Test 2: multiple rows insertion
DROP TABLE insert_test_id;
INSERT INTO insert_test VALUES (7), (8), (9);
SELECT create_provenance_mapping('insert_test_id', 'insert_test', 'id');
SELECT COUNT(*) FROM insert_test_id;
SELECT value, probability_evaluate(provenance) FROM insert_test_id ORDER BY value;

-- Test 3: insert token tracking/ logging into query_provenance table
CREATE TABLE query_provenance_result AS
SELECT query FROM query_provenance ORDER BY ts;
SELECT remove_provenance('query_provenance_result');
SELECT * FROM query_provenance_result;
DROP TABLE query_provenance_result;

DELETE FROM query_provenance;

DROP TABLE insert_test;
DROP TABLE insert_test_id;
