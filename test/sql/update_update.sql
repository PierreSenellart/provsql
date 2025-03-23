\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE update_test(id INT PRIMARY KEY);

INSERT INTO update_test (id)
VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('update_test');

DELETE FROM query_provenance;

-- Test 1: single row update
SELECT create_provenance_mapping('update_test_id', 'update_test', 'id');
SELECT COUNT(*) FROM update_test_id;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 1;

DROP TABLE update_test_id;
UPDATE update_test SET id = 6 WHERE id = 1;
SELECT create_provenance_mapping('update_test_id', 'update_test', 'id');
SELECT COUNT(*) FROM update_test_id;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 1;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 6;

-- Test 2: update of updated row (old row and new row)
DROP TABLE update_test_id;
UPDATE update_test SET id = 7 WHERE id = 1;
UPDATE update_test SET id = 8 WHERE id = 6;
SELECT create_provenance_mapping('update_test_id', 'update_test', 'id');
SELECT COUNT(*) FROM update_test_id;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 1;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 6;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 7;
SELECT probability_evaluate(provenance) FROM update_test_id WHERE value = 8;

-- Test 3: multiple rows deletion
DROP TABLE update_test_id;
UPDATE update_test SET id = id + 10 WHERE id >= 2 AND id <= 4;
SELECT create_provenance_mapping('update_test_id', 'update_test', 'id');
SELECT COUNT(*) FROM update_test_id;
SELECT value, probability_evaluate(provenance) FROM update_test_id ORDER BY value;

-- Test 4: update token tracking/ logging into query_provenance table
CREATE TABLE query_provenance_result AS
SELECT query FROM query_provenance ORDER BY ts;
SELECT remove_provenance('query_provenance_result');
SELECT * FROM query_provenance_result;
DROP TABLE query_provenance_result;

DELETE FROM query_provenance;

DROP TABLE update_test;
DROP TABLE update_test_id;
