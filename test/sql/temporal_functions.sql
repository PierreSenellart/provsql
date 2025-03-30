\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET TIME ZONE 'UTC';
SET datestyle = 'iso';

CREATE TABLE test (id INT, value INT);
SELECT add_provenance('test');

INSERT INTO test VALUES (1, 1), (2, 2), (3, 3);
DELETE FROM test WHERE id = 2;
UPDATE test SET value = 4 WHERE id = 3;

-- set fixed tstzmultirange values for predictable result
UPDATE query_provenance
SET valid_time = CASE
  WHEN query_type = 'INSERT' THEN tstzmultirange(tstzrange('1970-01-01 00:00:00+00', NULL))
  WHEN query_type = 'DELETE' THEN tstzmultirange(tstzrange('1970-01-01 00:00:01+00', NULL))
  WHEN query_type = 'UPDATE' THEN tstzmultirange(tstzrange('1970-01-01 00:00:02+00', NULL))
  ELSE valid_time
END
WHERE query_type IN ('INSERT', 'DELETE', 'UPDATE');

-- Test 1: get_valid_time
CREATE TABLE get_valid_time_result AS SELECT *, get_valid_time(provsql, 'test') AS valid_time FROM test;
SELECT remove_provenance('get_valid_time_result');
SELECT * FROM get_valid_time_result;
DROP TABLE get_valid_time_result;

CREATE TABLE get_valid_time_result AS SELECT * FROM test WHERE get_valid_time(provsql, 'test') @> CURRENT_TIMESTAMP;
SELECT remove_provenance('get_valid_time_result');
SELECT * FROM get_valid_time_result;
DROP TABLE get_valid_time_result;

-- Test 2: timetravel
CREATE TABLE timetravel_result AS SELECT * FROM timetravel('test', CURRENT_TIMESTAMP) AS tt(id int, value int, valid_time tstzmultirange, provsql uuid);
SELECT remove_provenance('timetravel_result');
SELECT * FROM timetravel_result;
DROP TABLE timetravel_result;

-- Test 3: timeslice
CREATE TABLE timeslice_result AS SELECT * FROM timeslice('test', CURRENT_TIMESTAMP - INTERVAL '1 hour', CURRENT_TIMESTAMP) AS (id int, value int, valid_time tstzmultirange, provsql uuid);;
SELECT remove_provenance('timeslice_result');
SELECT * FROM timeslice_result;
DROP TABLE timeslice_result;

-- Test 4: timeslice
CREATE TABLE history_result AS SELECT * FROM history('test', ARRAY['id'], ARRAY['3']) AS (id int, value int, valid_time tstzmultirange, provsql uuid);;
SELECT remove_provenance('history_result');
SELECT * FROM history_result;
DROP TABLE history_result;

DROP TABLE test;
DELETE FROM query_provenance;