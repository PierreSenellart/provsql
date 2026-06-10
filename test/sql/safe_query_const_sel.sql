\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Probes the safe-query read-once ('independent') rewrite in isolation;
-- pin the joint-width debug GUC off so its per-answer d-D does not replace
-- the read-once provenance the 'independent' method checks.
SET provsql.joint_width = off;

-- Constant-selection elimination (Dalvi & Suciu 2007 §5.1).
--
-- A WHERE clause @c Var @c = @c Const induces @c ∅ @c → @c Var.attno,
-- and the equijoin-closure union-find extends this FD to every Var in
-- the same equivalence class.  The safe-query detector's pre-pass
-- (apply_constant_selection_fd_pass) recognises this, propagates the
-- literal to every atom touched by the class, and drops the now-
-- redundant cross-atom equijoin conjuncts.  Constant-pinned atoms then
-- show up as separate components for the multi-component path, which
-- emits an independent gate_plus(atom_rows) factor at the top
-- @c gate_times -- exactly the read-once factoring constant-pinning
-- prescribes.
--
-- The canonical motivating example is the textbook H-shape:
--
--   SELECT DISTINCT 1
--     FROM R(x), S(x, y), T(y)
--    WHERE R.x = S.x AND S.y = T.y AND S.x = 42;
--
-- Without the pass: atoms(x)={R,S}, atoms(y)={S,T}, no class touches
-- every atom -> bail.  With the pass: S.x=42 pins class {R.x, S.x};
-- the equijoin R.x=S.x is dropped from the residual; R becomes its
-- own connected component (no remaining cross-atom links); the
-- multi-component path emits one inner sub-Query per component,
-- joined by gate_times at the outer.  R contributes
-- gate_plus(r.provsql) over its matching rows; the (S,T) component
-- contributes the standard hierarchical read-once shape
-- gate_plus(S.y * T.y) -- both factors share no leaves with each
-- other.

CREATE TABLE sq_const_r (x int);
CREATE TABLE sq_const_s (x int, y int);
CREATE TABLE sq_const_t (y int);

INSERT INTO sq_const_r VALUES (42), (42), (1);
INSERT INTO sq_const_s VALUES (42, 5), (42, 6), (1, 5);
INSERT INTO sq_const_t VALUES (5), (6), (7);

SELECT add_provenance('sq_const_r');
SELECT add_provenance('sq_const_s');
SELECT add_provenance('sq_const_t');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM sq_const_r;
  PERFORM set_prob(provsql, 0.5) FROM sq_const_s;
  PERFORM set_prob(provsql, 0.5) FROM sq_const_t;
END $$;

-- (1) Baseline without the rewrite: the H-query has a non-read-once
--     lineage on this instance, so 'independent' rejects.  The
--     default evaluator (d4 / tree decomposition) returns the exact
--     probability.
SET provsql.provenance = 'semiring';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (
        SELECT DISTINCT 1 AS x
          FROM sq_const_r r, sq_const_s s, sq_const_t t
         WHERE r.x = s.x AND s.y = t.y AND s.x = 42
      ) q;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the unrewritten '
                    'H-query circuit (shared R inputs across the cross product)';
  END IF;
END $$;

CREATE TEMP TABLE sq_const_baseline AS
  SELECT q.x, probability_evaluate(provenance()) AS p
    FROM (
      SELECT DISTINCT 1 AS x
        FROM sq_const_r r, sq_const_s s, sq_const_t t
       WHERE r.x = s.x AND s.y = t.y AND s.x = 42
    ) q
   GROUP BY q.x;
SELECT remove_provenance('sq_const_baseline');
SELECT x, ROUND(p::numeric, 6) AS prob_baseline FROM sq_const_baseline;

-- (2) With the rewrite active: the H-query is rewritten to a
--     read-once shape, and 'independent' succeeds.  Expected
--     probability matches the baseline within numerical noise (both
--     methods are exact on this small instance).
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sq_const_rewritten AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (
      SELECT DISTINCT 1 AS x
        FROM sq_const_r r, sq_const_s s, sq_const_t t
       WHERE r.x = s.x AND s.y = t.y AND s.x = 42
    ) q
   GROUP BY q.x;
SELECT remove_provenance('sq_const_rewritten');
SELECT x, ROUND(p::numeric, 6) AS prob_rewritten FROM sq_const_rewritten;

-- (3) The two probabilities must agree.
SELECT b.x,
       ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM sq_const_baseline b JOIN sq_const_rewritten r ON b.x = r.x;

-- (4) Constant on the other side of the equijoin: propagation runs
--     in the opposite direction (R.x = 42 pins S.x to 42 transitively
--     through the equijoin closure).  Result must match (2) exactly.
SET provsql.provenance = 'boolean';
CREATE TEMP TABLE sq_const_other_side AS
  SELECT q.x, probability_evaluate(provenance(), 'independent') AS p
    FROM (
      SELECT DISTINCT 1 AS x
        FROM sq_const_r r, sq_const_s s, sq_const_t t
       WHERE r.x = s.x AND s.y = t.y AND r.x = 42
    ) q
   GROUP BY q.x;
SELECT remove_provenance('sq_const_other_side');
SELECT b.x,
       ROUND((b.p - r.p)::numeric, 9) AS diff_other_side_vs_rewritten
  FROM sq_const_rewritten b JOIN sq_const_other_side r ON b.x = r.x;

-- (5) Inequality predicate (Var > Const) does NOT induce @c ∅ @c →
--     @c Var.attno, so the constant-selection pass must NOT fire.
--     The H-query is still non-hierarchical; the rewriter falls
--     through.  'independent' rejects the resulting circuit.
SET provsql.provenance = 'boolean';
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM (
        SELECT DISTINCT 1 AS x
          FROM sq_const_r r, sq_const_s s, sq_const_t t
         WHERE r.x = s.x AND s.y = t.y AND s.x < 100
      ) q;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the H-query '
                    'under an inequality predicate (no constant pinning)';
  END IF;
END $$;

DROP TABLE sq_const_r, sq_const_s, sq_const_t CASCADE;
