\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Phase-3 slice 3a-min: column pushdown for hierarchical CQs where
-- every shared (multi-atom) equivalence class touches every atom.
-- Each atom's wrap then projects every shared class (root class at
-- output attno 1, other shared classes at 2..N), so the outer cross
-- product per group is one wrap row per atom and the resulting
-- circuit stays read-once.
--
-- Cases where some shared class touches only a strict subset of the
-- atoms (e.g. q :- A(x,y), B(x,y), C(x) with y in {A,B} only) need
-- a nested sub-query per non-root shared-class signature; the
-- detector bails on those for now (deferred to a multi-level slice).

CREATE TABLE pd_a(x int, y int);
CREATE TABLE pd_b(x int, y int);
CREATE TABLE pd_c(x int);

INSERT INTO pd_a VALUES (1, 10), (1, 10), (1, 11), (2, 20);
INSERT INTO pd_b VALUES (1, 10), (1, 11), (1, 11), (2, 20);
INSERT INTO pd_c VALUES (1), (1), (2);

SELECT add_provenance('pd_a');
SELECT add_provenance('pd_b');
SELECT add_provenance('pd_c');

DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_a;
  PERFORM set_prob(provsql, 0.4) FROM pd_b;
  PERFORM set_prob(provsql, 0.6) FROM pd_c;
END $$;

-- (1) Two-atom CQ with two shared classes (x and y both touch
--     {pd_a, pd_b}).  Before slice 3a, the y class would be rejected
--     because it is not the root class.  The rewritten provenance
--     must match the baseline produced by the default evaluator on
--     the unrewritten (shared-input) circuit.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base1 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_a a, pd_b b
   WHERE a.x = b.x AND a.y = b.y
   GROUP BY a.x;
SELECT remove_provenance('pd_base1');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew1 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_a a, pd_b b
   WHERE a.x = b.x AND a.y = b.y
   GROUP BY a.x;
SELECT remove_provenance('pd_rew1');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base1 b JOIN pd_rew1 r ON b.x = r.x
 ORDER BY b.x;

-- (2) Three-atom CQ with a partial-coverage shared class (y only in
--     {pd_a, pd_b}).  Slice 3a-min must bail; the resulting circuit
--     is the unrewritten one, which is not read-once, so
--     'independent' rejects it.
SET provsql.boolean_provenance = on;
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM pd_a a, pd_b b, pd_c c
     WHERE a.x = b.x AND a.x = c.x AND a.y = b.y
     GROUP BY a.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the partial-coverage '
                    'CQ -- slice 3a-min should have bailed';
  END IF;
END $$;

-- (3) Rewritten root-gate shape for case (1): for each x, the root
--     must be gate_times of two gate_plus children (one per atom).
SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_shape AS
  SELECT a.x,
         get_gate_type(provenance())                  AS root,
         array_length(get_children(provenance()), 1)  AS root_nchildren
    FROM pd_a a, pd_b b
   WHERE a.x = b.x AND a.y = b.y
   GROUP BY a.x;
SELECT remove_provenance('pd_shape');
SELECT x, root, root_nchildren FROM pd_shape ORDER BY x;

-- (4) Non-hierarchical CQ must bail.  Classes X = {a.x, c.x},
--     Y = {a.y, b.y}, Z = {b.x, c.x_x} form a cycle (canonical "bad"
--     shape).  We do not test the bail directly (no observable hook
--     into the detector); instead we exercise the query and confirm
--     that 'independent' rejects the unrewritten circuit -- meaning
--     the rewriter did not produce a read-once factoring.
CREATE TABLE pd_d(x int, y int);
CREATE TABLE pd_e(x int, y int);
CREATE TABLE pd_f(x int, y int);
INSERT INTO pd_d VALUES (1, 10), (1, 11);
INSERT INTO pd_e VALUES (10, 100), (11, 100);
INSERT INTO pd_f VALUES (1, 100), (1, 200);
SELECT add_provenance('pd_d');
SELECT add_provenance('pd_e');
SELECT add_provenance('pd_f');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_d;
  PERFORM set_prob(provsql, 0.4) FROM pd_e;
  PERFORM set_prob(provsql, 0.6) FROM pd_f;
END $$;

SET provsql.boolean_provenance = on;
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM pd_d d, pd_e e, pd_f f
     WHERE d.y = e.x AND d.x = f.x AND e.y = f.y
     GROUP BY d.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the non-hierarchical '
                    'CQ -- the safe-query rewriter should have bailed';
  END IF;
END $$;

SET provsql.boolean_provenance = off;
DROP TABLE pd_a, pd_b, pd_c, pd_d, pd_e, pd_f;
