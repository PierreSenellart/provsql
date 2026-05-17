\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Exercises the ProcessUtility CTAS lineage hook: when a
-- CREATE TABLE AS / CREATE MATERIALIZED VIEW / SELECT INTO inner
-- SELECT projects a provsql column verbatim from a tracked source,
-- the hook populates the new relation's table_info (with the
-- inherited TID/BID kind and BID block-key alignment when the key
-- columns survive the projection) and ancestor set (transitive
-- union of source ancestors), and installs the provenance_guard
-- trigger so subsequent INSERT / UPDATE OF provsql still flip the
-- table to OPAQUE.
--
-- Output is shaped to be OID-stable: we compare ancestor lists
-- against the source relation's OID rather than printing raw OIDs.

CREATE TABLE ci_src_a(x int, y int);
INSERT INTO ci_src_a VALUES (1, 10), (2, 20), (3, 30);
SELECT add_provenance('ci_src_a');

CREATE TABLE ci_src_b(x int, lbl text);
INSERT INTO ci_src_b VALUES (1, 'b1'), (2, 'b2');
SELECT add_provenance('ci_src_b');

CREATE TABLE ci_bid(k int, v int);
INSERT INTO ci_bid VALUES (1, 100), (1, 101), (2, 200);
SELECT repair_key('ci_bid', 'k');

-- ---------------------------------------------------------------
-- (1) Single-source TID CTAS that projects the source's provsql:
--     inherits kind=TID, ancestors={ci_src_a}.
-- ---------------------------------------------------------------
CREATE TABLE ci_t1 AS SELECT x, provsql FROM ci_src_a;
SELECT (get_table_info('ci_t1'::regclass::oid)).kind  AS t1_kind,
       get_ancestors('ci_t1'::regclass::oid)
         = ARRAY['ci_src_a'::regclass::oid]           AS t1_ancestors_correct;

-- ---------------------------------------------------------------
-- (2) Multi-source TID CTAS (join over two disjoint TID sources)
--     projecting one source's provsql: classifier promotes to TID
--     (every source TID, ancestor sets {ci_src_a} and {ci_src_b}
--     pairwise disjoint).  The CTAS hook fires and records
--     kind=TID, ancestors=union({ci_src_a, ci_src_b}).
-- ---------------------------------------------------------------
CREATE TABLE ci_t2 AS
  SELECT a.x, a.provsql FROM ci_src_a a, ci_src_b b WHERE a.x = b.x;
SELECT (get_table_info('ci_t2'::regclass::oid)).kind  AS t2_kind,
       (SELECT array_agg(o ORDER BY o)
          FROM unnest(get_ancestors('ci_t2'::regclass::oid)) o)
       = (SELECT array_agg(o ORDER BY o)
            FROM unnest(ARRAY['ci_src_a'::regclass::oid,
                              'ci_src_b'::regclass::oid]) o)
                                                       AS t2_ancestors_correct;

-- ---------------------------------------------------------------
-- (3) CTAS without projecting provsql: the new relation has no
--     provsql column, so the hook skips and no metadata is recorded.
-- ---------------------------------------------------------------
CREATE TABLE ci_t3 AS SELECT x, y FROM ci_src_a;
SELECT get_table_info('ci_t3'::regclass::oid) IS NULL AS t3_no_metadata,
       get_ancestors('ci_t3'::regclass::oid) IS NULL  AS t3_no_ancestry;

-- ---------------------------------------------------------------
-- (4) BID source: target list keeps the block-key column k.
--     Inherits kind=BID with the new attno of k as block_key.
--     Original k is attno 1 in ci_bid; in the new ci_t4 it lands
--     at attno 1 too (we project k first), so block_key = {1}.
-- ---------------------------------------------------------------
CREATE TABLE ci_t4 AS SELECT k, provsql FROM ci_bid;
SELECT (get_table_info('ci_t4'::regclass::oid)).kind       AS t4_kind,
       (get_table_info('ci_t4'::regclass::oid)).block_key  AS t4_block_key,
       get_ancestors('ci_t4'::regclass::oid)
         = ARRAY['ci_bid'::regclass::oid]                  AS t4_ancestors_correct;

-- ---------------------------------------------------------------
-- (5) BID source: target list DROPS the block-key column.  Hook
--     demotes to TID rather than asserting a phantom block key.
-- ---------------------------------------------------------------
CREATE TABLE ci_t5 AS SELECT v, provsql FROM ci_bid;
SELECT (get_table_info('ci_t5'::regclass::oid)).kind       AS t5_kind_demoted,
       (get_table_info('ci_t5'::regclass::oid)).block_key  AS t5_block_key_empty;

-- ---------------------------------------------------------------
-- (6) SELECT INTO: PG's parser transforms this to CreateTableAsStmt;
--     the hook fires identically.
-- ---------------------------------------------------------------
SELECT x, provsql INTO ci_t6 FROM ci_src_a;
SELECT (get_table_info('ci_t6'::regclass::oid)).kind  AS t6_kind,
       get_ancestors('ci_t6'::regclass::oid)
         = ARRAY['ci_src_a'::regclass::oid]           AS t6_ancestors_correct;

-- ---------------------------------------------------------------
-- (7) CREATE MATERIALIZED VIEW: same statement node (CreateTableAsStmt
--     with objtype=OBJECT_MATVIEW), hook fires identically.
-- ---------------------------------------------------------------
CREATE MATERIALIZED VIEW ci_mv1 AS SELECT x, provsql FROM ci_src_a;
SELECT (get_table_info('ci_mv1'::regclass::oid)).kind  AS mv_kind,
       get_ancestors('ci_mv1'::regclass::oid)
         = ARRAY['ci_src_a'::regclass::oid]            AS mv_ancestors_correct;
DROP MATERIALIZED VIEW ci_mv1;

-- ---------------------------------------------------------------
-- (8) CTAS WITH NO DATA: structure-only, inner SELECT not executed.
--     The hook still fires (the new relation has a provsql column
--     by virtue of the TLE shape), so the metadata is recorded and
--     subsequent INSERTs will exercise provenance_guard.
-- ---------------------------------------------------------------
CREATE TABLE ci_t8 AS SELECT x, provsql FROM ci_src_a WITH NO DATA;
SELECT (get_table_info('ci_t8'::regclass::oid)).kind  AS t8_kind,
       get_ancestors('ci_t8'::regclass::oid)
         = ARRAY['ci_src_a'::regclass::oid]           AS t8_ancestors_correct;

-- ---------------------------------------------------------------
-- (9) provenance_guard installation: confirm the trigger exists on
--     a hook-tracked CTAS, and that supplying a user UUID on INSERT
--     flips the table to OPAQUE (the same guard semantics as
--     add_provenance'd tables).
-- ---------------------------------------------------------------
SELECT tgname FROM pg_trigger
 WHERE tgrelid = 'ci_t1'::regclass
   AND NOT tgisinternal
 ORDER BY tgname;
INSERT INTO ci_t1(x, provsql) VALUES (4, public.uuid_generate_v4());
SELECT (get_table_info('ci_t1'::regclass::oid)).kind  AS t1_kind_after_user_insert;

-- ---------------------------------------------------------------
-- (10) DROP TABLE fires cleanup_table_info, ancestry goes with it.
-- ---------------------------------------------------------------
SELECT 'ci_t4'::regclass::oid AS ci_t4_oid \gset
DROP TABLE ci_t4;
SELECT get_table_info(:ci_t4_oid) IS NULL AS t4_cleaned_up,
       get_ancestors(:ci_t4_oid) IS NULL  AS t4_ancestry_cleaned_up;

DROP TABLE ci_t1;
DROP TABLE ci_t2;
DROP TABLE ci_t3;
DROP TABLE ci_t5;
DROP TABLE ci_t6;
DROP TABLE ci_t8;
DROP TABLE ci_src_a;
DROP TABLE ci_src_b;
DROP TABLE ci_bid;
