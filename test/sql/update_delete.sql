\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.update_provenance = on;

CREATE TABLE delete_test(id INT PRIMARY KEY);

INSERT INTO delete_test (id)
VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('delete_test');

DELETE FROM update_provenance;

-- Test 1: single row deletion
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id = 1;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

-- Test 2: deletion of deleted row
DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id = 1;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

-- Test 3: multiple rows deletion
DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id >= 2 AND id <= 4;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT value, probability_evaluate(provenance) FROM delete_test_id ORDER BY value;

-- Test 4: delete token tracking/ logging into update_provenance table
CREATE TABLE update_provenance_result AS
SELECT query FROM update_provenance ORDER BY ts;
SELECT remove_provenance('update_provenance_result');
SELECT * FROM update_provenance_result;
DROP TABLE update_provenance_result;

DELETE FROM update_provenance;

DROP TABLE delete_test;
DROP TABLE delete_test_id;

SET provsql.update_provenance = off;
