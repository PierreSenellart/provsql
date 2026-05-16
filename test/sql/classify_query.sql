\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Exercises the query-time TID / BID / OPAQUE classifier surfaced
-- through the provsql.classify_top_level GUC.  When on, every
-- top-level SELECT that touches a relation emits a NOTICE of the
-- form
--
--    ProvSQL: query result is <KIND> (sources: schema.t1, ...)
--    ProvSQL: query result is <KIND> (no provenance-tracked sources)
--
-- where <KIND> is TID, BID, or OPAQUE under the existing
-- provsql_table_kind taxonomy.  Initial scope : single base relation
-- in a flat fromlist with no SubLinks, no modifying CTEs, no set
-- operations, no joins.  Everything outside that gate is reported
-- as OPAQUE.
--
-- The classifier is gated on the executor not being already entered,
-- so PL/pgSQL bodies the rewriter calls into (e.g. provenance_times)
-- do not produce spurious extra NOTICEs.
--
-- Each result-bearing query is captured into a TEMP table via the
-- standard CREATE TEMP TABLE AS / remove_provenance idiom so its
-- rows can be printed deterministically (the random input-gate
-- UUIDs that ProvSQL attaches to query results would otherwise
-- diff across runs).

-- Demo relations -----------------------------------------------------

-- TID (the default after add_provenance).
CREATE TABLE cq_tid (id int, label text);
INSERT INTO cq_tid VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT add_provenance('cq_tid');

-- BID with a single-column block key.
CREATE TABLE cq_bid (k int, v text);
INSERT INTO cq_bid VALUES (1, 'x'), (1, 'y'), (2, 'z');
SELECT repair_key('cq_bid', 'k');

-- OPAQUE via the set_table_info escape hatch.
CREATE TABLE cq_opaque (id int);
INSERT INTO cq_opaque VALUES (10), (20);
SELECT add_provenance('cq_opaque');
SELECT set_table_info('cq_opaque'::regclass::oid, 'opaque');

-- A plain PostgreSQL table with no provenance tracking.
CREATE TABLE cq_plain (id int);
INSERT INTO cq_plain VALUES (100);

-- (0) GUC default is off : the CTAS source SELECT goes through the
--     planner hook, but the gate is closed so no NOTICE fires.
CREATE TEMP TABLE cq_off AS SELECT id FROM cq_tid;
SELECT remove_provenance('cq_off');
DROP TABLE cq_off;

-- Switch on for everything below.
SET provsql.classify_top_level = on;

-- (1) Single-source TID, identity projection.
CREATE TEMP TABLE cq_r1 AS SELECT id FROM cq_tid;
SELECT remove_provenance('cq_r1');
SELECT id FROM cq_r1 ORDER BY id;

-- (2) Single-source TID under projection and WHERE.
CREATE TEMP TABLE cq_r2 AS SELECT label FROM cq_tid WHERE id = 2;
SELECT remove_provenance('cq_r2');
SELECT label FROM cq_r2;

-- (3) Single-source BID under identity.
CREATE TEMP TABLE cq_r3 AS SELECT k, v FROM cq_bid;
SELECT remove_provenance('cq_r3');
SELECT k, v FROM cq_r3 ORDER BY k, v;

-- (4) Single-source OPAQUE : the recorded kind propagates.
CREATE TEMP TABLE cq_r4 AS SELECT id FROM cq_opaque;
SELECT remove_provenance('cq_r4');
SELECT id FROM cq_r4 ORDER BY id;

-- (5) Plain (un-tracked) base relation : deterministic result, hence
--     trivially TID, with an empty sources list.
CREATE TEMP TABLE cq_r5 AS SELECT id FROM cq_plain;
SELECT id FROM cq_r5 ORDER BY id;

-- (6) Join of two TID tables : initial scope cannot certify
--     independence yet, so the result is OPAQUE; the sources list
--     names both tracked relations.
CREATE TEMP TABLE cq_r6 AS
  SELECT a.id, b.k FROM cq_tid a, cq_bid b WHERE a.id = b.k;
SELECT remove_provenance('cq_r6');
SELECT id, k FROM cq_r6 ORDER BY id, k;

-- (7) FROM-less SELECT : rtable is empty, so the classifier stays
--     silent (no NOTICE).
SELECT 1 + 1 AS two;

-- (8) GUC off again : no NOTICE.
SET provsql.classify_top_level = off;
CREATE TEMP TABLE cq_off2 AS SELECT id FROM cq_tid;
SELECT remove_provenance('cq_off2');
DROP TABLE cq_off2;

-- Cleanup ------------------------------------------------------------
SELECT remove_provenance('cq_tid');
SELECT remove_provenance('cq_bid');
SELECT remove_provenance('cq_opaque');
DROP TABLE cq_tid, cq_bid, cq_opaque, cq_plain;
