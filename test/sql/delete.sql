\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

CREATE TABLE t(id INT PRIMARY KEY);

INSERT INTO t (id) 
VALUES (1), (2), (3), (4), (5);

SELECT add_provenance('t');

-- Test 1: single row deletion
SELECT create_provenance_mapping('t_id', 't', 'id');
SELECT COUNT(*) FROM t_id;
SELECT probability_evaluate(provenance) FROM t_id WHERE value = 1;

DROP TABLE t_id;
DELETE FROM t WHERE id = 1;
SELECT create_provenance_mapping('t_id', 't', 'id');
SELECT COUNT(*) FROM t_id;
SELECT probability_evaluate(provenance) FROM t_id WHERE value = 1;

-- Test 2: multiple rows deletion
DROP TABLE t_id;
SELECT create_provenance_mapping('t_id', 't', 'id');
SELECT COUNT(*) FROM t_id;
SELECT probability_evaluate(provenance) FROM t_id WHERE value = 1;

DROP TABLE t_id;
DELETE FROM t WHERE id >= 2 AND id <= 4;
SELECT create_provenance_mapping('t_id', 't', 'id');
SELECT COUNT(*) FROM t_id;
SELECT probability_evaluate(provenance) FROM t_id WHERE value = 1;

-- Test 3: delete token tracking/ logging into delete_provenance table
SELECT COUNT(*) FROM delete_provenance;

