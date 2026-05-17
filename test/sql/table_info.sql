\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Exercises the per-table provenance metadata store added for the
-- safe-query optimisation: add_provenance / repair_key /
-- remove_provenance / DROP-TABLE event-trigger paths must all keep
-- get_table_info(relid) in sync, including the kind enum
-- (tid / bid / opaque) and multi-column block keys.

-- (1) After add_provenance: kind=tid, empty block_key
CREATE TABLE ti_basic(a int, b int);
SELECT add_provenance('ti_basic');
SELECT kind, block_key FROM get_table_info('ti_basic'::regclass::oid);

-- (2) After remove_provenance: get_table_info returns NULL
SELECT remove_provenance('ti_basic');
SELECT get_table_info('ti_basic'::regclass::oid) IS NULL AS removed;

-- (3) repair_key with a single-column key:
--     kind=bid, block_key=[attnum(a)] = [1]
CREATE TABLE ti_single(a int, b int);
INSERT INTO ti_single VALUES (1,10),(1,11),(2,20);
SELECT repair_key('ti_single', 'a');
SELECT kind, block_key FROM get_table_info('ti_single'::regclass::oid);

-- (4) repair_key with a multi-column key:
--     kind=bid, block_key=[attnum(a), attnum(b)] = [1, 2]
CREATE TABLE ti_multi(a int, b int, c int);
INSERT INTO ti_multi VALUES (1,1,10),(1,1,11),(1,2,20),(2,2,30);
SELECT repair_key('ti_multi', 'a, b');
SELECT kind, block_key FROM get_table_info('ti_multi'::regclass::oid);

-- (5) repair_key with the empty key (whole table is one block):
--     kind=bid, block_key=empty -- distinguishable from kind=opaque
--     (which the safe-query rewriter must bail on).
CREATE TABLE ti_one(a int);
INSERT INTO ti_one VALUES (1),(2);
SELECT repair_key('ti_one', '');
SELECT kind, block_key FROM get_table_info('ti_one'::regclass::oid);

-- (6) Direct set_table_info('opaque', ...) for the derived-table case
--     (CREATE TABLE AS SELECT, INSERT INTO SELECT, UPDATE under
--     provsql.update_provenance).  The metadata layer must round-trip
--     this kind even though no high-level SQL helper writes it yet.
CREATE TABLE ti_opaque(a int);
SELECT add_provenance('ti_opaque');
SELECT set_table_info('ti_opaque'::regclass::oid, 'opaque');
SELECT kind, block_key FROM get_table_info('ti_opaque'::regclass::oid);

-- (7) DROP TABLE fires the cleanup_table_info event trigger
SELECT 'ti_multi'::regclass::oid AS ti_multi_oid \gset
DROP TABLE ti_multi;
SELECT get_table_info(:ti_multi_oid) IS NULL AS cleaned_up_by_event_trigger;

DROP TABLE ti_basic;
DROP TABLE ti_single;
DROP TABLE ti_one;
DROP TABLE ti_opaque;
