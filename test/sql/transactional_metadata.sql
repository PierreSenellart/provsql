\set ECHO none
\pset format unaligned

-- Per-relation provenance metadata lives in the provsql.table_info heap
-- table, so every change to it follows the transaction that made it.
-- When it lived in an mmap file outside PostgreSQL's control, none of
-- the checks below held.

CREATE TABLE tm_base (name text);
INSERT INTO tm_base VALUES ('alice'), ('bob');
SELECT add_provenance('tm_base');

SELECT (get_table_info('tm_base'::regclass::oid)).kind AS base_kind;
SELECT get_ancestors('tm_base'::regclass::oid) = ARRAY['tm_base'::regclass::oid]
  AS base_ancestry;

SELECT 'tm_base'::regclass::oid AS base_oid \gset

-- A rolled-back add_provenance leaves no row behind.
BEGIN;
CREATE TABLE tm_rolled (name text);
SELECT add_provenance('tm_rolled');
SELECT (get_table_info('tm_rolled'::regclass::oid)).kind AS rolled_kind_in_txn;
SELECT 'tm_rolled'::regclass::oid AS rolled_oid \gset
ROLLBACK;
SELECT count(*) AS rolled_rows_left
  FROM provsql.table_info WHERE relid::oid = :rolled_oid;

-- A rolled-back DROP TABLE keeps the relation's row: when the sql_drop
-- event trigger wrote to the mmap store, its removal survived the
-- rollback and the relation came back untracked.
BEGIN;
DROP TABLE tm_base;
SELECT count(*) AS rows_inside_drop
  FROM provsql.table_info WHERE relid::oid = :base_oid;
ROLLBACK;
SELECT (get_table_info(:base_oid)).kind AS kind_after_drop_rollback;

-- A rolled-back user-supplied token leaves the relation TID: the guard
-- trigger's flip to OPAQUE rolls back with the INSERT that caused it.
BEGIN;
INSERT INTO tm_base(name, provsql) VALUES ('mallory', public.uuid_generate_v4());
SELECT (get_table_info(:base_oid)).kind AS kind_in_guard_txn;
ROLLBACK;
SELECT (get_table_info(:base_oid)).kind AS kind_after_guard_rollback;

-- Committed, the same flip stands.
INSERT INTO tm_base(name, provsql) VALUES ('mallory', public.uuid_generate_v4());
SELECT (get_table_info(:base_oid)).kind AS kind_after_guard_commit;

-- A committed DROP TABLE still removes the row.
DROP TABLE tm_base;
SELECT count(*) AS rows_after_drop
  FROM provsql.table_info WHERE relid::oid = :base_oid;

-- migrate_table_info is idempotent and a no-op on a database with no
-- legacy file: a fresh install, and any database already migrated.
SELECT migrate_table_info() AS legacy_rows_imported;
SELECT migrate_table_info() AS legacy_rows_imported_again;
