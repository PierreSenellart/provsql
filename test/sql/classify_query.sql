\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Exercises the query-time TID / BID / OPAQUE classifier surfaced
-- through the provsql.classify_top_level GUC.  When on, every
-- top-level SELECT that touches a relation emits a NOTICE of the
-- form
--
--    ProvSQL: query result is TID (sources: schema.t1, ...)
--    ProvSQL: query result is TID (no provenance-tracked sources)
--    ProvSQL: query result is BID (sources: schema.t1)
--    ProvSQL: query result is OPAQUE
--
-- TID and BID carry a complete source list ; OPAQUE deliberately
-- omits the parenthetical because when the shape gate trips the
-- source list would be partial and misleading (the rtable walk
-- only reaches syntactically visible RTEs, missing relations
-- referenced from inside e.g. a SubLink).
--
-- <KIND> is TID, BID, or OPAQUE under the existing
-- provsql_table_kind taxonomy.  Scope :
--
--  * Single-source : a flat fromlist of RangeTblRefs with no
--    kind-altering features (SubLinks, modifying CTEs, DISTINCT,
--    GROUP BY, HAVING, aggregates, window functions, SRFs in the
--    target list), one tracked relation reached either directly or
--    through any depth of RTE_SUBQUERY entries.  ORDER BY, LIMIT,
--    OFFSET are transparent.
--  * UNION ALL : a fully-UNION-ALL tree of subquery legs, each
--    independently TID, over pairwise-disjoint relid sets, promotes
--    to TID with the cumulative source list.
--  * Zero tracked sources : trivially TID, empty source list.
--  * Everything else : OPAQUE.
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

-- A second TID relation, used in the UNION ALL disjoint-source case.
CREATE TABLE cq_tid2 (n int, lbl text);
INSERT INTO cq_tid2 VALUES (10, 'x'), (20, 'y');
SELECT add_provenance('cq_tid2');

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

-- (7) Inline subquery over a TID base relation : the classifier
--     descends through RTE_SUBQUERY, finds cq_tid as the sole
--     tracked source, and propagates TID.
CREATE TEMP TABLE cq_r7 AS
  SELECT id FROM (SELECT id, label FROM cq_tid WHERE id < 3) s;
SELECT remove_provenance('cq_r7');
SELECT id FROM cq_r7 ORDER BY id;

-- (8) Inline subquery over a BID base relation : kind preserved
--     through descent.
CREATE TEMP TABLE cq_r8 AS
  SELECT k FROM (SELECT k, v FROM cq_bid) s;
SELECT remove_provenance('cq_r8');
SELECT k FROM cq_r8 ORDER BY k;

-- (9) Doubly-nested inline subqueries over a TID base relation :
--     recursion handles arbitrary depth.
CREATE TEMP TABLE cq_r9 AS
  SELECT id FROM (SELECT id FROM (SELECT * FROM cq_tid) s1) s2;
SELECT remove_provenance('cq_r9');
SELECT id FROM cq_r9 ORDER BY id;

-- (10) Inline subquery wrapping a two-source join : the inner
--      sources are both surfaced; the cumulative source count is
--      two, so the outer classifies as OPAQUE.
CREATE TEMP TABLE cq_r10 AS
  SELECT id, k FROM (
    SELECT a.id, b.k FROM cq_tid a, cq_bid b WHERE a.id = b.k
  ) s;
SELECT remove_provenance('cq_r10');
SELECT id, k FROM cq_r10 ORDER BY id, k;

-- (11) Outer joining an inline subquery with a base relation :
--      sources from both layers contribute, total > 1, OPAQUE.
CREATE TEMP TABLE cq_r11 AS
  SELECT s.id, b.k FROM (SELECT id FROM cq_tid) s, cq_bid b
  WHERE s.id = b.k;
SELECT remove_provenance('cq_r11');
SELECT id, k FROM cq_r11 ORDER BY id, k;

-- Transparent operators --------------------------------------------
-- ORDER BY, LIMIT, OFFSET do not change row lineages; the
-- classifier should look through them and inherit the source's
-- recorded kind.  (The result-printing queries below remove_provenance
-- on the materialised temp first, then SELECT plain rows -- no
-- aggregate -- so the verification step itself classifies as TID
-- with no tracked sources rather than producing extra spurious
-- NOTICEs.)

-- (12) TID under ORDER BY.
CREATE TEMP TABLE cq_r12 AS SELECT id FROM cq_tid ORDER BY id DESC;
SELECT remove_provenance('cq_r12');
SELECT id FROM cq_r12 ORDER BY id;

-- (13) TID under LIMIT.
CREATE TEMP TABLE cq_r13 AS SELECT id FROM cq_tid ORDER BY id LIMIT 2;
SELECT remove_provenance('cq_r13');
SELECT id FROM cq_r13 ORDER BY id;

-- (14) TID under OFFSET.
CREATE TEMP TABLE cq_r14 AS SELECT id FROM cq_tid ORDER BY id OFFSET 1;
SELECT remove_provenance('cq_r14');
SELECT id FROM cq_r14 ORDER BY id;

-- Lineage-altering operators ----------------------------------------
-- Each of these turns per-row lineage into a composite (an OR of
-- input atoms for DISTINCT / GROUP BY, a sum over a group for
-- aggregates, a per-row function of multiple inputs for window
-- functions, an atom-sharing fan-out for SRFs).  None of them
-- preserve the per-row independent-atom property TID demands, so
-- the classifier rejects them and reports OPAQUE while still
-- enumerating the visible tracked sources for diagnostics.

-- (15) DISTINCT.
CREATE TEMP TABLE cq_r15 AS SELECT DISTINCT label FROM cq_tid;
SELECT remove_provenance('cq_r15');
SELECT label FROM cq_r15 ORDER BY label;

-- (16) GROUP BY (no aggregate body needed for the gate trip).
CREATE TEMP TABLE cq_r16 AS SELECT label FROM cq_tid GROUP BY label;
SELECT remove_provenance('cq_r16');
SELECT label FROM cq_r16 ORDER BY label;

-- (17) GROUP BY + HAVING + aggregate, all three features rolled
--      into one case to keep the test count tractable.
CREATE TEMP TABLE cq_r17 AS
  SELECT label, count(*) AS c FROM cq_tid GROUP BY label HAVING count(*) >= 1;
SELECT remove_provenance('cq_r17');
SELECT label, c FROM cq_r17 ORDER BY label;

-- (18) Window function in the target list.
CREATE TEMP TABLE cq_r18 AS
  SELECT id, row_number() OVER (ORDER BY id) AS rn FROM cq_tid;
SELECT remove_provenance('cq_r18');
SELECT id, rn FROM cq_r18 ORDER BY id;

-- (19) Set-returning function in the target list.
CREATE TEMP TABLE cq_r19 AS
  SELECT generate_series(1, 2) AS g FROM cq_tid;
SELECT remove_provenance('cq_r19');
SELECT g FROM cq_r19 ORDER BY g;

-- UNION ALL specialisation ------------------------------------------
-- A fully-UNION-ALL tree of TID legs over pairwise-disjoint relid
-- sets promotes to TID with the cumulative source list.  Anything
-- else (UNION DISTINCT, INTERSECT, EXCEPT, mixed leg kinds, or
-- overlapping leg sources) falls back to OPAQUE.  UNION ALL of
-- compatible BID legs is deliberately deferred ; see
-- doc/TODO/safe-query-followups.md.

-- (20) Disjoint TIDs : the union is TID with both sources listed.
CREATE TEMP TABLE cq_r20 AS
  SELECT id AS x FROM cq_tid UNION ALL SELECT n AS x FROM cq_tid2;
SELECT remove_provenance('cq_r20');
SELECT x FROM cq_r20 ORDER BY x;

-- (21) Same TID on both legs : the relid is shared between legs,
--      so the gate-level atoms are not disjoint -- OPAQUE.
CREATE TEMP TABLE cq_r21 AS
  SELECT id FROM cq_tid WHERE id = 1
  UNION ALL
  SELECT id FROM cq_tid WHERE id = 2;
SELECT remove_provenance('cq_r21');
SELECT id FROM cq_r21 ORDER BY id;

-- (22) UNION (DISTINCT, the default without ALL) : the implicit
--      duplicate-elimination introduces an OR over rows, so the
--      union is not TID.  OPAQUE.
CREATE TEMP TABLE cq_r22 AS
  SELECT id AS x FROM cq_tid UNION SELECT n AS x FROM cq_tid2;
SELECT remove_provenance('cq_r22');
SELECT x FROM cq_r22 ORDER BY x;

-- (INTERSECT is omitted here : ProvSQL's rewriter rejects it
-- ahead of the classifier with "Set operations other than UNION
-- and EXCEPT not supported", so the CTAS errors out and the test
-- output becomes noisy.  The classifier's INTERSECT path falls
-- through to OPAQUE, identical to UNION DISTINCT and EXCEPT above
-- and below.)

-- (23) EXCEPT : OPAQUE.
CREATE TEMP TABLE cq_r23 AS
  SELECT id AS x FROM cq_tid EXCEPT SELECT n AS x FROM cq_tid2;
SELECT remove_provenance('cq_r23');
SELECT x FROM cq_r23 ORDER BY x;

-- (24) Heterogeneous kinds (TID UNION ALL BID) : not promoted.
--      The current rule requires every leg to be TID ; BID legs
--      fall through to OPAQUE.
CREATE TEMP TABLE cq_r24 AS
  SELECT id AS x FROM cq_tid UNION ALL SELECT k AS x FROM cq_bid;
SELECT remove_provenance('cq_r24');
SELECT x FROM cq_r24 ORDER BY x;

-- (25) UNION ALL of a TID leg and a plain (untracked) leg : the
--      plain leg classifies as TID with no sources, so the union's
--      cumulative source set is {cq_tid}, kind TID.
CREATE TEMP TABLE cq_r25 AS
  SELECT id AS x FROM cq_tid UNION ALL SELECT id AS x FROM cq_plain;
SELECT remove_provenance('cq_r25');
SELECT x FROM cq_r25 ORDER BY x;

-- (26) Three-leg UNION ALL : TID + TID + plain over disjoint
--      tracked relids.  Tests that nested SetOperationStmt nodes
--      are flattened correctly.
CREATE TEMP TABLE cq_r26 AS
  SELECT id AS x FROM cq_tid
  UNION ALL
  SELECT n AS x FROM cq_tid2
  UNION ALL
  SELECT id AS x FROM cq_plain;
SELECT remove_provenance('cq_r26');
SELECT x FROM cq_r26 ORDER BY x;

-- (27) FROM-less SELECT : rtable is empty, so the classifier stays
--      silent (no NOTICE).
SELECT 1 + 1 AS two;

-- (28) GUC off again : no NOTICE.
SET provsql.classify_top_level = off;
CREATE TEMP TABLE cq_off2 AS SELECT id FROM cq_tid;
SELECT remove_provenance('cq_off2');
DROP TABLE cq_off2;

-- Cleanup ------------------------------------------------------------
SELECT remove_provenance('cq_tid');
SELECT remove_provenance('cq_tid2');
SELECT remove_provenance('cq_bid');
SELECT remove_provenance('cq_opaque');
DROP TABLE cq_tid, cq_tid2, cq_bid, cq_opaque, cq_plain;
