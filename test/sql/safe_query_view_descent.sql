\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Safe-query rewriter -- subquery-inlining pre-pass.  PG inlines a
-- view into the outer Query as an RTE_SUBQUERY before the planner
-- hook runs; without the pre-pass the candidate gate refuses on the
-- non-RTE_RELATION rtable entry.  With the pre-pass the view body's
-- RTEs are lifted into the outer rtable, the candidate gate sees a
-- flat range table of base relations, and the existing hierarchical-
-- CQ detector fires.
--
-- Coverage:
--  (V1) trivial @c SELECT @c * view.
--  (V2) view + base-table join (the typical case).
--  (V3) view body carrying its own WHERE conjunct (merged with the
--       outer WHERE during inlining).
--  (V4) projection with column rename.
--  (V5) two-level nesting (view over a view), fixed-point loop.
--  (V6) view that itself joins two tracked tables.
--  (V7) negative: view body with GROUP BY (inlineable predicate
--       rejects -> pre-pass no-op -> candidate gate refuses).
--  (V8) negative: view + base table that share an underlying relid
--       (shared-relid bail in candidate gate after inlining).

CREATE TABLE vd_a(x int);
CREATE TABLE vd_b(x int);
CREATE TABLE vd_c(x int);

INSERT INTO vd_a VALUES (1), (1), (2);
INSERT INTO vd_b VALUES (1), (2), (2);
INSERT INTO vd_c VALUES (1), (1), (2), (3);

SELECT add_provenance('vd_a');
SELECT add_provenance('vd_b');
SELECT add_provenance('vd_c');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM vd_a;
  PERFORM set_prob(provsql, 0.4) FROM vd_b;
  PERFORM set_prob(provsql, 0.6) FROM vd_c;
END $$;

-- Probe: given a query expression, return per-x whether the
-- rewriter fired and the probability under the rewritten / baseline
-- evaluators (rounded so floating-point noise doesn't drift the
-- expected output).  Mirrors the opq_probe idiom from
-- safe_query_opaque.sql but with the additional probability cross-
-- check.
CREATE OR REPLACE FUNCTION vd_probe(qtxt text)
  RETURNS TABLE(x int, rewriter text, p_rew numeric, p_base numeric) AS $$
DECLARE rec record;
BEGIN
  SET LOCAL provsql.provenance = 'boolean';
  EXECUTE format($f$
    CREATE TEMP TABLE vd_rew ON COMMIT DROP AS
      SELECT q.x, provsql.provenance() AS p FROM (%s) q GROUP BY q.x
  $f$, qtxt);
  PERFORM provsql.remove_provenance('vd_rew');

  SET LOCAL provsql.provenance = 'semiring';
  EXECUTE format($f$
    CREATE TEMP TABLE vd_base ON COMMIT DROP AS
      SELECT q.x, provsql.provenance() AS p FROM (%s) q GROUP BY q.x
  $f$, qtxt);
  PERFORM provsql.remove_provenance('vd_base');

  FOR rec IN
    SELECT r.x,
           CASE WHEN provsql.get_gate_type(r.p)::text = 'assumed'
                THEN 'fired' ELSE 'refused' END AS rewriter,
           ROUND(provsql.probability_evaluate(r.p)::numeric, 6) AS p_rew,
           ROUND(provsql.probability_evaluate(b.p)::numeric, 6) AS p_base
      FROM vd_rew r JOIN vd_base b ON r.x = b.x
     ORDER BY r.x
  LOOP
    x        := rec.x;
    rewriter := rec.rewriter;
    p_rew    := rec.p_rew;
    p_base   := rec.p_base;
    RETURN NEXT;
  END LOOP;
  DROP TABLE vd_rew;
  DROP TABLE vd_base;
END;
$$ LANGUAGE plpgsql;

-- ---------------------------------------------------------------
-- (V1) Trivial SELECT * view -- nothing for the inliner to merge
-- beyond the RTE itself.  Rewriter sees a one-atom query, which is
-- accepted by the deterministic-source branch of the detector.
-- ---------------------------------------------------------------
CREATE VIEW vd_v1 AS SELECT * FROM vd_a;
SELECT 'V1' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v1 t, vd_b u WHERE t.x = u.x $$);
DROP VIEW vd_v1;

-- ---------------------------------------------------------------
-- (V2) Standard 2-atom hierarchical CQ where one side comes
-- through a view.
-- ---------------------------------------------------------------
CREATE VIEW vd_v2 AS SELECT * FROM vd_a;
SELECT 'V2' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v2 t, vd_b u WHERE t.x = u.x $$);
DROP VIEW vd_v2;

-- ---------------------------------------------------------------
-- (V3) View body carries its own WHERE; after inlining the predicate
-- merges into the outer WHERE.  The output row count for x=2 stays
-- 1, the rewriter's per-x probability matches the baseline.
-- ---------------------------------------------------------------
CREATE VIEW vd_v3 AS SELECT * FROM vd_a WHERE x <= 2;
SELECT 'V3' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v3 t, vd_b u WHERE t.x = u.x $$);
DROP VIEW vd_v3;

-- ---------------------------------------------------------------
-- (V4) View renames its column.  The inliner substitutes the outer
-- Var with the TLE Var pointing at the underlying column; the
-- rename is purely syntactic and disappears in the inlined query.
-- ---------------------------------------------------------------
CREATE VIEW vd_v4(renamed) AS SELECT a.x FROM vd_a a;
SELECT 'V4' AS scenario, * FROM vd_probe(
  $$ SELECT t.renamed AS x FROM vd_v4 t, vd_b u WHERE t.renamed = u.x $$);
DROP VIEW vd_v4;

-- ---------------------------------------------------------------
-- (V5) View over a view.  The first iteration of the fixed-point
-- loop inlines vd_v5_outer; the second iteration sees vd_v5_inner
-- newly exposed in the fromlist and inlines it too.
-- ---------------------------------------------------------------
CREATE VIEW vd_v5_inner AS SELECT * FROM vd_a;
CREATE VIEW vd_v5_outer AS SELECT * FROM vd_v5_inner;
SELECT 'V5' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v5_outer t, vd_b u WHERE t.x = u.x $$);
DROP VIEW vd_v5_outer;
DROP VIEW vd_v5_inner;

-- ---------------------------------------------------------------
-- (V6) View body is itself a 2-atom join.  After inlining, the
-- outer query becomes a 3-atom hierarchical CQ.
-- ---------------------------------------------------------------
CREATE VIEW vd_v6 AS SELECT a.x FROM vd_a a, vd_b b WHERE a.x = b.x;
SELECT 'V6' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v6 t, vd_c u WHERE t.x = u.x $$);
DROP VIEW vd_v6;

-- ---------------------------------------------------------------
-- (V7) Negative: view body uses GROUP BY.  The inlineable predicate
-- refuses (the subquery's @c groupClause changes row lineage), the
-- pre-pass returns NULL, and the candidate gate then refuses on the
-- still-present RTE_SUBQUERY.
-- ---------------------------------------------------------------
CREATE VIEW vd_v7 AS SELECT a.x FROM vd_a a GROUP BY a.x;
SELECT 'V7' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v7 t, vd_b u WHERE t.x = u.x $$);
DROP VIEW vd_v7;

-- ---------------------------------------------------------------
-- (V8) Negative: view + base table that share an underlying relid.
-- After inlining the outer rtable contains vd_a (from the view body)
-- and vd_a (from the outer FROM); the candidate gate's shared-relid
-- bail refuses.  The PK / disjoint-constant self-join pre-passes
-- don't apply (no PK on vd_a, no @c x @c = @c const conjuncts).
-- ---------------------------------------------------------------
CREATE VIEW vd_v8 AS SELECT * FROM vd_a;
SELECT 'V8' AS scenario, * FROM vd_probe(
  $$ SELECT t.x FROM vd_v8 t, vd_a u WHERE t.x = u.x $$);
DROP VIEW vd_v8;

DROP FUNCTION vd_probe(text);
DROP TABLE vd_a;
DROP TABLE vd_b;
DROP TABLE vd_c;
