\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Audit: every documented and undocumented way to obtain a table with
-- a provsql column whose values are NOT independent.  The safe-query
-- rewriter is only sound on TID / BID tables that came through
-- add_provenance / repair_key; for anything else it must refuse to
-- fire (per-row root stays an ordinary gate_times / gate_plus, never
-- gate_assumed_boolean).
--
-- This file pins the contract.  Each scenario carries a DESIRED:
-- note explaining what the rewriter should do; the expected output
-- encodes that contract verbatim.  When a scenario's current
-- behaviour disagrees with its DESIRED note, the test fails -- by
-- design -- until the detector is fixed.

-- ---------------------------------------------------------------
-- Reference tracked tables used as the "right side" of every join.
-- ---------------------------------------------------------------
CREATE TABLE opq_ref(x int, lbl text);
INSERT INTO opq_ref VALUES (1, 'r1'), (2, 'r2');
SELECT add_provenance('opq_ref');
DO $$ BEGIN PERFORM set_prob(provsql, 0.4) FROM opq_ref; END $$;

-- Same shape, used as a source table for INSERT-FROM-SELECT and
-- CTAS scenarios.
CREATE TABLE opq_src(x int, lbl text);
INSERT INTO opq_src VALUES (1, 's1'), (2, 's2');
SELECT add_provenance('opq_src');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM opq_src; END $$;

-- (Helper opq_probe is defined below, just before SCENARIO 1.)

-- ---------------------------------------------------------------
-- Helper: run the canonical 2-atom hierarchical CQ on (lhs, opq_ref)
-- under boolean_provenance=on and report, per output x, whether the
-- rewriter fired ('fired' if the per-row root is gate_assumed_boolean,
-- 'refused' otherwise).  The exact non-marker root type
-- (gate_times / gate_plus / ...) is incidental, so we collapse it.
-- ---------------------------------------------------------------
CREATE OR REPLACE FUNCTION opq_probe(lhs regclass)
  RETURNS TABLE(x int, rewriter text) AS $$
DECLARE rec record;
BEGIN
  EXECUTE format($f$
    CREATE TEMP TABLE opq_probe_res ON COMMIT DROP AS
      SELECT a.x AS x, provsql.provenance() AS p
        FROM %s a, provsql_test.opq_ref b WHERE a.x = b.x
       GROUP BY a.x
  $f$, lhs);
  PERFORM provsql.remove_provenance('opq_probe_res');
  FOR rec IN SELECT r.x, provsql.get_gate_type(r.p)::text AS gt
               FROM opq_probe_res r ORDER BY r.x LOOP
    x := rec.x;
    rewriter := CASE WHEN rec.gt = 'assumed_boolean'
                     THEN 'fired' ELSE 'refused' END;
    RETURN NEXT;
  END LOOP;
  DROP TABLE opq_probe_res;
END;
$$ LANGUAGE plpgsql;

SET provsql.boolean_provenance = on;

-- ---------------------------------------------------------------
-- (S0) Direct INSERT with two rows sharing the same provsql UUID.
--      add_provenance no longer installs a UNIQUE constraint; the
--      INSERTs succeed and the provenance_guard trigger flips the
--      table's metadata to OPAQUE on the first user-supplied
--      provsql value.
--
-- DESIRED: refuse (table is now OPAQUE).
-- ---------------------------------------------------------------
CREATE TABLE opq_s0(x int);
SELECT add_provenance('opq_s0');
DO $$ DECLARE u uuid := public.uuid_generate_v4();
BEGIN
  INSERT INTO opq_s0(x, provsql) VALUES (1, u);
  INSERT INTO opq_s0(x, provsql) VALUES (2, u);
END $$;
SELECT 'S0' AS scenario, * FROM opq_probe('opq_s0');

-- ---------------------------------------------------------------
-- (S1) Cross-table UUID reuse via INSERT FROM SELECT.  opq_s1's
-- rows carry opq_src's UUIDs verbatim, so a join opq_s1 x opq_ref
-- still references opq_src's gates -- joining opq_s1 with opq_src
-- itself would expose the aliasing as a shared input that the
-- rewriter assumes is two independent leaves.
--
-- DESIRED: refuse (the detector has no way to know the source
--          table this came from; the safe move is to require that
--          rows landed in this table only through add_provenance's
--          DEFAULT, which we currently cannot enforce).
-- ---------------------------------------------------------------
CREATE TABLE opq_s1(x int);
SELECT add_provenance('opq_s1');
INSERT INTO opq_s1(x, provsql) SELECT x, provsql FROM opq_src;
SELECT 'S1' AS scenario, * FROM opq_probe('opq_s1');

-- ---------------------------------------------------------------
-- (S2) CREATE TABLE AS SELECT from a tracked table.  The new
-- table has a provsql column populated with opq_src's UUIDs but
-- no entry in provsql_table_info.mmap.
--
-- DESIRED: refuse (the table has a provsql column but no TID/BID
--          classification: the detector cannot prove independence).
-- ---------------------------------------------------------------
CREATE TABLE opq_s2 AS SELECT x, provsql FROM opq_src;
SELECT 'S2' AS scenario, * FROM opq_probe('opq_s2');

-- ---------------------------------------------------------------
-- (S3) CREATE VIEW that joins two tracked tables and projects
-- only one's provsql.  PG inlines the view as an RTE_SUBQUERY
-- before the planner hook sees the outer Query; the safe-query
-- subquery-inlining pre-pass lifts opq_src and opq_s3_aux back
-- into the outer rtable, so the opq_aux dependency is reconstructed
-- and the rewriter sees a 3-atom hierarchical CQ that is safe to
-- rewrite (opq_src, opq_s3_aux, opq_ref are independently tracked,
-- their base relids are disjoint, and the @c x variable is the
-- hierarchical root).  Row probabilities account for all three
-- atoms regardless of which provsql column the view chose to
-- project.
--
-- DESIRED: fired.
-- ---------------------------------------------------------------
CREATE TABLE opq_s3_aux(x int);
INSERT INTO opq_s3_aux VALUES (1), (2);
SELECT add_provenance('opq_s3_aux');
DO $$ BEGIN PERFORM set_prob(provsql, 0.3) FROM opq_s3_aux; END $$;
CREATE VIEW opq_s3 AS
  SELECT a.x, a.provsql FROM opq_src a, opq_s3_aux b WHERE a.x = b.x;
SELECT 'S3' AS scenario, * FROM opq_probe('opq_s3');
DROP VIEW opq_s3;
DROP TABLE opq_s3_aux;

-- ---------------------------------------------------------------
-- (S4) Manually rename an arbitrary uuid column to 'provsql'.
-- No add_provenance was ever called; no UNIQUE constraint on the
-- renamed column; no metadata.  We seed it with two rows sharing
-- the same UUID -- a correlation the rewriter would assume away.
--
-- DESIRED: refuse (column exists but table is unregistered;
--          detector should treat as opaque, not deterministic).
-- ---------------------------------------------------------------
CREATE TABLE opq_s4(x int, mycol uuid);
DO $$ DECLARE u uuid := public.uuid_generate_v4();
BEGIN
  INSERT INTO opq_s4 VALUES (1, u), (2, u);
END $$;
ALTER TABLE opq_s4 RENAME COLUMN mycol TO provsql;
SELECT 'S4' AS scenario, * FROM opq_probe('opq_s4');

-- ---------------------------------------------------------------
-- (S5) Manually ALTER TABLE ADD COLUMN provsql uuid.  Same shape
-- as (S4) but without the rename trick.
--
-- DESIRED: refuse (same reason as S4).
-- ---------------------------------------------------------------
CREATE TABLE opq_s5(x int);
ALTER TABLE opq_s5 ADD COLUMN provsql uuid;
DO $$ DECLARE u uuid := public.uuid_generate_v4();
BEGIN
  INSERT INTO opq_s5 VALUES (1, u), (2, u);
END $$;
SELECT 'S5' AS scenario, * FROM opq_probe('opq_s5');

-- ---------------------------------------------------------------
-- (S6) Explicit set_table_info('opaque') flips a properly tracked
-- table into the OPAQUE class.  This is the one path the detector
-- already gates on, so we expect a refusal today.
--
-- DESIRED: refuse.
-- ---------------------------------------------------------------
CREATE TABLE opq_s6(x int);
SELECT add_provenance('opq_s6');
INSERT INTO opq_s6 VALUES (1), (2);
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM opq_s6; END $$;
SELECT provsql.set_table_info('opq_s6'::regclass::oid, 'opaque');
SELECT 'S6' AS scenario, * FROM opq_probe('opq_s6');

-- ---------------------------------------------------------------
-- (S7) UPDATE provsql to point at another row's UUID.  The
--      provenance_guard trigger now catches the UPDATE (NEW.provsql
--      DISTINCT FROM OLD.provsql) and flips the table to OPAQUE.
--
-- DESIRED: refuse (table is now OPAQUE).
-- ---------------------------------------------------------------
CREATE TABLE opq_s7(x int);
SELECT add_provenance('opq_s7');
INSERT INTO opq_s7 VALUES (1), (2);
DO $$ DECLARE other uuid;
BEGIN
  SELECT provsql INTO other FROM opq_s7 WHERE x = 2;
  UPDATE opq_s7 SET provsql = other WHERE x = 1;
END $$;
SELECT 'S7' AS scenario, * FROM opq_probe('opq_s7');

-- ---------------------------------------------------------------
-- (S8) INSERT INTO a properly-tracked table with a provsql value
-- that is structurally the root of an existing multi-input gate
-- (here, a hand-built gate_plus over two leaves of opq_src).
-- The UNIQUE constraint accepts each insert (the UUIDs are new in
-- opq_s8), but the row's provenance is now a compound expression
-- rather than an independent leaf -- precisely an opaque
-- correlation that the rewriter would treat as a fresh independent
-- gate_input.
--
-- DESIRED: refuse.
-- ---------------------------------------------------------------
CREATE TABLE opq_s8(x int);
SELECT add_provenance('opq_s8');
DO $$ DECLARE t1 uuid; t2 uuid; u1 uuid; u2 uuid;
BEGIN
  SELECT s.provsql INTO u1 FROM opq_src s WHERE s.x = 1;
  SELECT s.provsql INTO u2 FROM opq_src s WHERE s.x = 2;
  t1 := public.uuid_generate_v5(uuid_ns_provsql(), 'opq_s8_x1');
  t2 := public.uuid_generate_v5(uuid_ns_provsql(), 'opq_s8_x2');
  PERFORM create_gate(t1, 'plus', ARRAY[u1, u2]);
  PERFORM create_gate(t2, 'plus', ARRAY[u1, u2]);
  INSERT INTO opq_s8(x, provsql) VALUES (1, t1), (2, t2);
END $$;
SELECT 'S8' AS scenario, * FROM opq_probe('opq_s8');

-- ---------------------------------------------------------------
-- (S9) INSERT INTO a repair_key (BID) table with a fresh
-- user-supplied provsql value.  The repair_key path classifies
-- the table as BID with a specific block_key column set; the
-- per-block mulinput leaves are minted by repair_key itself, so
-- a later INSERT that supplies its own provsql breaks the
-- block-key invariant in the same way an INSERT on a TID table
-- breaks independence.  provenance_guard should flip to OPAQUE
-- the same way it does on a TID table.
--
-- DESIRED: refuse (table flipped to OPAQUE by the guard).
-- ---------------------------------------------------------------
CREATE TABLE opq_s9(x int, k int);
INSERT INTO opq_s9 VALUES (1, 10), (1, 11), (2, 20), (2, 21);
SELECT repair_key('opq_s9', 'x');
INSERT INTO opq_s9(x, k, provsql) VALUES (3, 30, public.uuid_generate_v4());
SELECT 'S9' AS scenario, * FROM opq_probe('opq_s9');

-- ---------------------------------------------------------------
-- (S10) UPDATE a BID table's provsql column to a fresh UUID.
-- Same rationale as S9; the guard should fire on UPDATE too.
--
-- DESIRED: refuse.
-- ---------------------------------------------------------------
CREATE TABLE opq_s10(x int, k int);
INSERT INTO opq_s10 VALUES (1, 10), (1, 11), (2, 20), (2, 21);
SELECT repair_key('opq_s10', 'x');
UPDATE opq_s10 SET provsql = public.uuid_generate_v4()
 WHERE k = 11;
SELECT 'S10' AS scenario, * FROM opq_probe('opq_s10');

DROP FUNCTION opq_probe(regclass);
SET provsql.boolean_provenance = off;

DROP TABLE opq_ref, opq_src,
           opq_s0, opq_s1, opq_s2, opq_s4, opq_s5,
           opq_s6, opq_s7, opq_s8, opq_s9, opq_s10;
