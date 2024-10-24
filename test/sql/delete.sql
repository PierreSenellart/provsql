\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE delete_test(id INT PRIMARY KEY);

INSERT INTO delete_test (id)
VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('delete_test');

-- Test 1: single row deletion
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id = 1;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

-- Test 2: multiple rows deletion
DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id = 1;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT probability_evaluate(provenance) FROM delete_test_id WHERE value = 1;

DROP TABLE delete_test_id;
DELETE FROM delete_test WHERE id >= 2 AND id <= 4;
SELECT create_provenance_mapping('delete_test_id', 'delete_test', 'id');
SELECT COUNT(*) FROM delete_test_id;
SELECT value, probability_evaluate(provenance) FROM delete_test_id ORDER BY value;

-- Test 3: delete token tracking/ logging into delete_provenance table
SELECT query FROM delete_provenance ORDER BY deleted_at;

DROP TABLE delete_test;
DROP TABLE delete_test_id;
