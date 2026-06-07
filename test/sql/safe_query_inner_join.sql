\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Safe-query rewriter -- INNER JoinExpr flattening pre-pass.
--
-- PG's ANSI-syntax JOIN constructs put a JoinExpr in the fromlist
-- instead of flat RangeTblRefs.  The pre-pass dissolves INNER and
-- CROSS joins into flat fromlist + AND-merged ON-clauses so the
-- candidate gate's flat-fromlist requirement matches the comma-
-- style FROM A, B counterpart.  Outer joins (LEFT / RIGHT / FULL),
-- aliased joins, and USING clauses are refused (pass-through) to
-- preserve gate-level semantics.

CREATE TABLE ij_a(x int);
CREATE TABLE ij_b(x int);
CREATE TABLE ij_c(x int);

INSERT INTO ij_a VALUES (1), (1), (2);
INSERT INTO ij_b VALUES (1), (2), (2);
INSERT INTO ij_c VALUES (1), (2), (3);

SELECT add_provenance('ij_a');
SELECT add_provenance('ij_b');
SELECT add_provenance('ij_c');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM ij_a;
  PERFORM set_prob(provsql, 0.4) FROM ij_b;
  PERFORM set_prob(provsql, 0.6) FROM ij_c;
END $$;

CREATE OR REPLACE FUNCTION ij_probe(qtxt text)
  RETURNS TABLE(x int, rewriter text, p_rew numeric, p_base numeric) AS $$
DECLARE rec record;
BEGIN
  SET LOCAL provsql.provenance = 'boolean';
  EXECUTE format($f$
    CREATE TEMP TABLE ij_rew ON COMMIT DROP AS
      SELECT q.x, provsql.provenance() AS p FROM (%s) q GROUP BY q.x
  $f$, qtxt);
  PERFORM provsql.remove_provenance('ij_rew');

  SET LOCAL provsql.provenance = 'semiring';
  EXECUTE format($f$
    CREATE TEMP TABLE ij_base ON COMMIT DROP AS
      SELECT q.x, provsql.provenance() AS p FROM (%s) q GROUP BY q.x
  $f$, qtxt);
  PERFORM provsql.remove_provenance('ij_base');

  FOR rec IN
    SELECT r.x,
           CASE WHEN provsql.get_gate_type(r.p)::text = 'assumed'
                THEN 'fired' ELSE 'refused' END AS rewriter,
           ROUND(provsql.probability_evaluate(r.p)::numeric, 6) AS p_rew,
           ROUND(provsql.probability_evaluate(b.p)::numeric, 6) AS p_base
      FROM ij_rew r JOIN ij_base b ON r.x = b.x
     ORDER BY r.x
  LOOP
    x        := rec.x;
    rewriter := rec.rewriter;
    p_rew    := rec.p_rew;
    p_base   := rec.p_base;
    RETURN NEXT;
  END LOOP;
  DROP TABLE ij_rew;
  DROP TABLE ij_base;
END;
$$ LANGUAGE plpgsql;

-- ---------------------------------------------------------------
-- (J1) ANSI INNER JOIN ... ON.  The JoinExpr is flattened into the
-- equivalent of FROM ij_a a, ij_b b WHERE a.x = b.x ; the rewriter
-- fires and per-x probabilities match the baseline.
-- ---------------------------------------------------------------
SELECT 'J1' AS scenario, * FROM ij_probe(
  $$ SELECT a.x FROM ij_a a INNER JOIN ij_b b ON a.x = b.x $$);

-- ---------------------------------------------------------------
-- (J2) Nested INNER JOIN (3-atom).  Multiple JoinExprs in one
-- fromlist entry flatten recursively into a 3-RangeTblRef fromlist
-- with the ON-clauses AND-merged.
-- ---------------------------------------------------------------
SELECT 'J2' AS scenario, * FROM ij_probe(
  $$ SELECT a.x FROM ij_a a INNER JOIN ij_b b ON a.x = b.x
                            INNER JOIN ij_c c ON a.x = c.x $$);

-- ---------------------------------------------------------------
-- (J3) CROSS JOIN (also JOIN_INNER in PG with empty ON).  Same
-- flattening path.  Probabilities match baseline.
-- ---------------------------------------------------------------
SELECT 'J3' AS scenario, * FROM ij_probe(
  $$ SELECT a.x FROM ij_a a CROSS JOIN ij_b b WHERE a.x = b.x $$);

-- ---------------------------------------------------------------
-- (J4) LEFT OUTER JOIN.  The outer-join lowering rewrites it into
-- the UNION of its matched and null-padded antijoin arms before the
-- safe-query pre-pass runs, so the candidate gate refuses on that
-- (now set-op) shape.  Probabilities are the correct LEFT-JOIN group
-- existence values P(some left row in the group): x=1 -> 1-0.5^2 =
-- 0.75, x=2 -> P(a2) = 0.5 (not the old inner-join 0.30 / 0.32).
-- ---------------------------------------------------------------
SELECT 'J4' AS scenario, * FROM ij_probe(
  $$ SELECT a.x FROM ij_a a LEFT JOIN ij_b b ON a.x = b.x $$);

-- ---------------------------------------------------------------
-- (J5) USING clause.  The synthetic join column drives
-- joinaliasvars magic that the conservative pre-pass refuses
-- rather than resolve through.
-- ---------------------------------------------------------------
SELECT 'J5' AS scenario, * FROM ij_probe(
  $$ SELECT q.x FROM (ij_a INNER JOIN ij_b USING (x)) q $$);

DROP FUNCTION ij_probe(text);
DROP TABLE ij_a;
DROP TABLE ij_b;
DROP TABLE ij_c;
