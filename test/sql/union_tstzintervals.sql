\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET provsql.update_provenance='on';

SET TIME ZONE 'UTC';
SET datestyle = 'iso';

DELETE FROM update_provenance;
CREATE TABLE test(id INT PRIMARY KEY);
SELECT add_provenance('test');

-- Test 1: test tuple valid time after insert, delete, update operations
INSERT INTO test (id) VALUES (1), (2), (3);
DELETE FROM test WHERE id = 2;
UPDATE test SET id = 4 WHERE id = 3;

-- set fixed tstzmultirange values for predictable result
UPDATE update_provenance
SET valid_time = CASE
  WHEN query_type = 'INSERT' THEN tstzmultirange(tstzrange('1970-01-01 00:00:00+00', NULL))
  WHEN query_type = 'DELETE' THEN tstzmultirange(tstzrange('1970-01-01 00:00:01+00', NULL))
  WHEN query_type = 'UPDATE' THEN tstzmultirange(tstzrange('1970-01-01 00:00:02+00', NULL))
  ELSE valid_time
END
WHERE query_type IN ('INSERT', 'DELETE', 'UPDATE');

CREATE TABLE union_tstzintervals_result AS SELECT *, union_tstzintervals(provenance(),'provsql.time_validity_view') FROM test;
SELECT remove_provenance('union_tstzintervals_result');
SELECT * FROM union_tstzintervals_result;
DROP TABLE union_tstzintervals_result;

-- Test 2: test tuple valid time after undo operation
CREATE TABLE undo_result AS SELECT undo(provsql) FROM update_provenance WHERE query = 'DELETE FROM test WHERE id = 2;';
DROP TABLE undo_result;

-- set fixed tstzmultirange values for predictable result
UPDATE update_provenance
SET valid_time = tstzmultirange(tstzrange('1970-01-01 00:00:03+00', NULL))
WHERE query_type = 'UNDO';

CREATE TABLE union_tstzintervals_result AS SELECT *, union_tstzintervals(provenance(),'provsql.time_validity_view') FROM test;
SELECT remove_provenance('union_tstzintervals_result');
SELECT * FROM union_tstzintervals_result;
DROP TABLE union_tstzintervals_result;

-- Test 3: insert token tracking/ logging into update_provenance table
CREATE TABLE update_provenance_result AS
SELECT query FROM update_provenance ORDER BY ts;
SELECT remove_provenance('update_provenance_result');
SELECT * FROM update_provenance_result;
DROP TABLE update_provenance_result;

DELETE FROM update_provenance;
DROP TABLE test;

-- Test 4: GROUP BY with ORDER BY on union_tstzintervals result (exercises
-- the provenance_function_in_group_by fix: ORDER BY must not suppress
-- GROUP BY aggregation of provenance)
CREATE TABLE test_grp(id INT PRIMARY KEY, cat TEXT);
SELECT add_provenance('test_grp');
-- Insert separately so each row gets its own update_provenance entry
INSERT INTO test_grp (id, cat) VALUES (1, 'A');
INSERT INTO test_grp (id, cat) VALUES (2, 'A');
INSERT INTO test_grp (id, cat) VALUES (3, 'B');

-- Assign fixed intervals in insertion order (rn 1→id=1, 2→id=2, 3→id=3)
WITH ranked AS (
  SELECT provsql, ROW_NUMBER() OVER (ORDER BY ts) AS rn
  FROM update_provenance
  WHERE query_type = 'INSERT'
)
UPDATE update_provenance up
SET valid_time = CASE r.rn
  WHEN 1 THEN tstzmultirange(tstzrange('1970-01-01 00:00:00+00', '1970-01-01 00:00:02+00'))
  WHEN 2 THEN tstzmultirange(tstzrange('1970-01-01 00:00:04+00', '1970-01-01 00:00:06+00'))
  WHEN 3 THEN tstzmultirange(tstzrange('1970-01-01 00:00:01+00', '1970-01-01 00:00:03+00'))
END
FROM ranked r
WHERE up.provsql = r.provsql;

CREATE TABLE test_grp_result AS
  SELECT cat, union_tstzintervals(provenance(), 'provsql.time_validity_view') AS valid
  FROM test_grp
  GROUP BY cat ORDER BY valid;
SELECT remove_provenance('test_grp_result');
SELECT * FROM test_grp_result ORDER BY valid;
DROP TABLE test_grp_result;

DELETE FROM update_provenance;
DROP TABLE test_grp;

SET provsql.update_provenance='off';
