\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

SET client_min_messages = WARNING;
DROP DATABASE IF EXISTS provsql_test_aux;
RESET client_min_messages;

-- Test 1: Circuit isolation across databases
SELECT get_nb_gates() AS gates_before \gset

CREATE DATABASE provsql_test_aux;
\c provsql_test_aux

CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION provsql;
CREATE SCHEMA provsql_test;
SET search_path TO provsql_test, provsql;

CREATE TABLE t (x int);
SELECT add_provenance('t');
INSERT INTO t VALUES (1), (2), (3);
DO $$ BEGIN PERFORM 1 FROM t; END $$;

SELECT get_nb_gates() > 0 AS aux_has_gates;

-- Test 2: mmap files are created per-database and removed with the database
\c contrib_regression
SET search_path TO provsql_test, provsql;

SELECT get_nb_gates() = :gates_before AS primary_unaffected;

SELECT oid::text AS aux_oid
FROM pg_database
WHERE datname = 'provsql_test_aux' \gset

SELECT count(*) AS mmap_files
FROM pg_ls_dir('base/' || :'aux_oid')
WHERE pg_ls_dir LIKE 'provsql_%.mmap';

DROP DATABASE provsql_test_aux;

SELECT count(*) AS aux_dir_gone
FROM pg_ls_dir('base')
WHERE pg_ls_dir = :'aux_oid';
