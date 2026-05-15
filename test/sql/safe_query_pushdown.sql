\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- Safe-query rewriter -- column and atom-local qual pushdown for
-- hierarchical CQs.
--
-- When every shared (multi-atom) equivalence class touches every
-- atom, each atom's wrap projects every shared class (root class at
-- output attno 1, other shared classes at 2..N), the outer cross
-- product per group is one wrap row per atom, and the resulting
-- circuit stays read-once.
--
-- When some shared class touches only a strict subset of the atoms
-- (e.g. q :- A(x,y), B(x,y), C(x) with y in {A,B} only), the
-- detector bundles those atoms into an inner sub-Query whose
-- @c GROUP @c BY folds the partial-coverage variable away before the
-- outer join.

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
--     {pd_a, pd_b}).  The wrap of each atom projects both x and y;
--     the rewritten provenance must match the baseline produced by
--     the default evaluator on the unrewritten (shared-input)
--     circuit.
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
--     {pd_a, pd_b}).  The multi-level path builds an inner sub-Query
--     that folds y for {pd_a, pd_b} before joining the outer C-wrap
--     on x, so the resulting circuit is read-once and the rewritten
--     probability must match the baseline.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base2 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_a a, pd_b b, pd_c c
   WHERE a.x = b.x AND a.x = c.x AND a.y = b.y
   GROUP BY a.x;
SELECT remove_provenance('pd_base2');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew2 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_a a, pd_b b, pd_c c
   WHERE a.x = b.x AND a.x = c.x AND a.y = b.y
   GROUP BY a.x;
SELECT remove_provenance('pd_rew2');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base2 b JOIN pd_rew2 r ON b.x = r.x
 ORDER BY b.x;

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

-- (4) Case A: A(x,y), B(x,y), C(x,y,z) with c.z unreferenced
--     anywhere in the outer query.  PostgreSQL's parser never
--     materialises a Var for c.z, so the detector only sees x and y
--     -- both shared classes touch all three atoms.  The rewriter
--     accepts and the rewritten circuit must be read-once with the
--     same probability as the baseline.
CREATE TABLE pd_g(x int, y int, z int);
INSERT INTO pd_g VALUES (1, 10, 100), (1, 10, 200), (1, 11, 100), (2, 20, 999);
SELECT add_provenance('pd_g');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.7) FROM pd_g;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base4 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_a a, pd_b b, pd_g g
   WHERE a.x = b.x AND a.x = g.x AND a.y = b.y AND a.y = g.y
   GROUP BY a.x;
SELECT remove_provenance('pd_base4');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew4 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_a a, pd_b b, pd_g g
   WHERE a.x = b.x AND a.x = g.x AND a.y = b.y AND a.y = g.y
   GROUP BY a.x;
SELECT remove_provenance('pd_rew4');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base4 b JOIN pd_rew4 r ON b.x = r.x
 ORDER BY b.x;

-- (5) Case B: A(x,y), B(x,y), C(x,y,z) WHERE c.z > 100.
--     c.z is a single-atom existential variable that only appears in
--     a top-level pushable conjunct.  The atom-local pre-pass
--     extracts the conjunct into C's inner wrap (so the wrap becomes
--     SELECT DISTINCT x, y, provsql FROM pd_g WHERE z > 100), and
--     the residual outer query no longer references c.z.  The
--     detector then sees only x and y, both shared classes touching
--     all three atoms, and accepts.  The rewritten probability must
--     match the baseline.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base5 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_a a, pd_b b, pd_g g
   WHERE a.x = b.x AND a.x = g.x AND a.y = b.y AND a.y = g.y
     AND g.z > 100
   GROUP BY a.x;
SELECT remove_provenance('pd_base5');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew5 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_a a, pd_b b, pd_g g
   WHERE a.x = b.x AND a.x = g.x AND a.y = b.y AND a.y = g.y
     AND g.z > 100
   GROUP BY a.x;
SELECT remove_provenance('pd_rew5');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base5 b JOIN pd_rew5 r ON b.x = r.x
 ORDER BY b.x;

-- (7) Multi-level rewrite + atom-local qual pushdown on a grouped
--     atom: same shape as (2) with @c a.y > 10 added.  The y-Var of
--     pd_a only appears inside the predicate and inside the a.y=b.y
--     equality; the atom-local pre-pass pushes the @c >10 conjunct
--     into pd_a's pushed_quals, and the multi-level builder carries
--     that conjunct into the inner sub-Query's WHERE.  When the
--     rewriter re-enters on the inner sub-Query, the atom-local
--     pre-pass re-extracts the conjunct into pd_a's wrap inside the
--     inner.  Rewritten probability must match the baseline.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base7 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_a a, pd_b b, pd_c c
   WHERE a.x = b.x AND a.x = c.x AND a.y = b.y
     AND a.y > 10
   GROUP BY a.x;
SELECT remove_provenance('pd_base7');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew7 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_a a, pd_b b, pd_c c
   WHERE a.x = b.x AND a.x = c.x AND a.y = b.y
     AND a.y > 10
   GROUP BY a.x;
SELECT remove_provenance('pd_rew7');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base7 b JOIN pd_rew7 r ON b.x = r.x
 ORDER BY b.x;

-- (8) Multi-level rewrite + extra fully-covered class.  Atoms
--     pd_p(x,w,y) and pd_q(x,w,y) share root @c x, fully-covered
--     non-root @c w, and partial-coverage @c y; pd_r(x,w) is the
--     outer atom that joins on both @c x and @c w.  The inner sub-
--     Query must expose @em both @c x @em and @c w in its targetList
--     and @c GROUP @c BY (x, w), so the outer can join the inner
--     with pd_r's wrap on (x, w).  Without this, the bare slice-3c
--     code would either bail or produce a wrong remap.
CREATE TABLE pd_p(x int, w int, y int);
CREATE TABLE pd_q(x int, w int, y int);
CREATE TABLE pd_r(x int, w int);
INSERT INTO pd_p VALUES (1, 100, 10), (1, 100, 11), (1, 200, 10), (2, 300, 20);
INSERT INTO pd_q VALUES (1, 100, 10), (1, 100, 11), (1, 200, 11), (2, 300, 20);
INSERT INTO pd_r VALUES (1, 100), (1, 200), (2, 300);
SELECT add_provenance('pd_p');
SELECT add_provenance('pd_q');
SELECT add_provenance('pd_r');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_p;
  PERFORM set_prob(provsql, 0.4) FROM pd_q;
  PERFORM set_prob(provsql, 0.6) FROM pd_r;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base8 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_p a, pd_q b, pd_r c
   WHERE a.x = b.x AND a.w = b.w AND a.y = b.y
     AND a.x = c.x AND a.w = c.w
   GROUP BY a.x;
SELECT remove_provenance('pd_base8');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew8 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_p a, pd_q b, pd_r c
   WHERE a.x = b.x AND a.w = b.w AND a.y = b.y
     AND a.x = c.x AND a.w = c.w
   GROUP BY a.x;
SELECT remove_provenance('pd_rew8');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base8 b JOIN pd_rew8 r ON b.x = r.x
 ORDER BY b.x;

-- (9) Non-flat (nested) partial-coverage signatures:
--       pd_n_a(x,y,z), pd_n_b(x,y,z), pd_n_c(x,y), pd_n_d(x)
--     atoms(x)={A,B,C,D}, atoms(y)={A,B,C}, atoms(z)={A,B}.
--     Signatures: A:{y,z}, B:{y,z}, C:{y}, D:{}.  Two distinct
--     non-empty signatures.  D has an empty signature so the
--     recursion terminates: the outermost peel bundles {A,B,C} into
--     one inner sub-Query on x; Choice A re-entry then sees y as
--     fully covered within {A,B,C} and z as the new partial-coverage
--     class, peeling {A,B} into another inner sub-Query.  Rewritten
--     probability must match the baseline.
CREATE TABLE pd_n_a(x int, y int, z int);
CREATE TABLE pd_n_b(x int, y int, z int);
CREATE TABLE pd_n_c(x int, y int);
CREATE TABLE pd_n_d(x int);
INSERT INTO pd_n_a VALUES (1,10,100),(1,10,101),(1,11,100),(2,20,200);
INSERT INTO pd_n_b VALUES (1,10,100),(1,10,101),(1,11,100),(2,20,200);
INSERT INTO pd_n_c VALUES (1,10),(1,11),(2,20);
INSERT INTO pd_n_d VALUES (1),(2);
SELECT add_provenance('pd_n_a');
SELECT add_provenance('pd_n_b');
SELECT add_provenance('pd_n_c');
SELECT add_provenance('pd_n_d');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_n_a;
  PERFORM set_prob(provsql, 0.4) FROM pd_n_b;
  PERFORM set_prob(provsql, 0.6) FROM pd_n_c;
  PERFORM set_prob(provsql, 0.7) FROM pd_n_d;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base9 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_n_a a, pd_n_b b, pd_n_c c, pd_n_d d
   WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
     AND a.y = b.y AND a.y = c.y
     AND a.z = b.z
   GROUP BY a.x;
SELECT remove_provenance('pd_base9');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew9 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_n_a a, pd_n_b b, pd_n_c c, pd_n_d d
   WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
     AND a.y = b.y AND a.y = c.y
     AND a.z = b.z
   GROUP BY a.x;
SELECT remove_provenance('pd_rew9');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base9 b JOIN pd_rew9 r ON b.x = r.x
 ORDER BY b.x;

-- (10) Disjoint partial-coverage signatures (multi-group at the same
--      level): pd_dj_a(x,y), pd_dj_b(x,y), pd_dj_c(x,z), pd_dj_d(x,z).
--      atoms(y)={A,B}, atoms(z)={C,D}; no atom has an empty signature
--      but the signatures partition cleanly.  The rewriter must build
--      TWO inner sub-Queries at the outermost level, joined on x.
CREATE TABLE pd_dj_a(x int, y int);
CREATE TABLE pd_dj_b(x int, y int);
CREATE TABLE pd_dj_c(x int, z int);
CREATE TABLE pd_dj_d(x int, z int);
INSERT INTO pd_dj_a VALUES (1,10),(1,11),(2,20),(2,20);
INSERT INTO pd_dj_b VALUES (1,10),(1,11),(2,20),(2,20);
INSERT INTO pd_dj_c VALUES (1,100),(1,101),(2,200);
INSERT INTO pd_dj_d VALUES (1,100),(1,101),(2,200);
SELECT add_provenance('pd_dj_a');
SELECT add_provenance('pd_dj_b');
SELECT add_provenance('pd_dj_c');
SELECT add_provenance('pd_dj_d');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_dj_a;
  PERFORM set_prob(provsql, 0.4) FROM pd_dj_b;
  PERFORM set_prob(provsql, 0.6) FROM pd_dj_c;
  PERFORM set_prob(provsql, 0.7) FROM pd_dj_d;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base10 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_dj_a a, pd_dj_b b, pd_dj_c c, pd_dj_d d
   WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
     AND a.y = b.y AND c.z = d.z
   GROUP BY a.x;
SELECT remove_provenance('pd_base10');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew10 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_dj_a a, pd_dj_b b, pd_dj_c c, pd_dj_d d
   WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
     AND a.y = b.y AND c.z = d.z
   GROUP BY a.x;
SELECT remove_provenance('pd_rew10');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base10 b JOIN pd_rew10 r ON b.x = r.x
 ORDER BY b.x;

-- (11) BID block-key alignment.
--    a. Aligned: pd_bid_a is BID with block_key={x}; the rewriter wraps
--       it with proj_slots={x}, so block_key is fully covered.  The
--       rewrite proceeds and the rewritten probability must match the
--       baseline.  pd_bid_p is duplicated per x so the baseline
--       circuit shares pd_bid_p's gate_inputs across A-rows at the
--       same x (independent rejects the unrewritten circuit; the
--       rewritten one is read-once).
--    b. Misaligned: pd_bid_b is BID with block_key={y}, y is not
--       referenced anywhere in the query so it is not in proj_slots.
--       The rewriter must bail; independent then rejects the
--       unrewritten circuit because of the same pd_bid_p sharing.
CREATE TABLE pd_bid_a(x int, y int);
CREATE TABLE pd_bid_p(x int);
INSERT INTO pd_bid_a VALUES (1, 10), (1, 11), (2, 20);
INSERT INTO pd_bid_p VALUES (1), (1), (2);
SELECT repair_key('pd_bid_a', 'x');
SELECT add_provenance('pd_bid_p');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_bid_p;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_bid_base AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_bid_a a, pd_bid_p p
   WHERE a.x = p.x
   GROUP BY a.x;
SELECT remove_provenance('pd_bid_base');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_bid_rew AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_bid_a a, pd_bid_p p
   WHERE a.x = p.x
   GROUP BY a.x;
SELECT remove_provenance('pd_bid_rew');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_bid_base b JOIN pd_bid_rew r ON b.x = r.x
 ORDER BY b.x;

CREATE TABLE pd_bid_b(x int, y int);
INSERT INTO pd_bid_b VALUES (1, 100), (1, 200), (2, 300);
SELECT repair_key('pd_bid_b', 'y');

SET provsql.boolean_provenance = on;
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM pd_bid_b b, pd_bid_p p
     WHERE b.x = p.x
     GROUP BY b.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the misaligned BID '
                    'query -- the rewriter should have bailed';
  END IF;
END $$;

-- (12) Empty block_key: whole table is one block (all tuples mutually
--      exclusive).  The wrap's DISTINCT could split that block across
--      multiple output rows whenever the table has more than one row;
--      the rewriter must bail conservatively.
CREATE TABLE pd_bid_empty(x int);
INSERT INTO pd_bid_empty VALUES (1), (2), (3);
SELECT repair_key('pd_bid_empty', '');

SET provsql.boolean_provenance = on;
DO $$
DECLARE raised boolean := false;
BEGIN
  BEGIN
    PERFORM probability_evaluate(provenance(), 'independent')
      FROM pd_bid_empty a, pd_bid_p p
     WHERE a.x = p.x
     GROUP BY a.x;
  EXCEPTION WHEN OTHERS THEN raised := true;
  END;
  IF NOT raised THEN
    RAISE EXCEPTION 'expected ''independent'' to reject the empty-block_key '
                    'BID query -- the rewriter should have bailed';
  END IF;
END $$;

-- (13) Multi-component (disconnected) CQ:
--      pd_mc_a(x), pd_mc_b(y), no join condition.  Two independent
--      components; the rewriter builds one inner sub-Query per
--      component and Cartesian-products them at the outer.  Each
--      output row's provsql is gate_times of two per-component
--      gate_pluses, which is read-once per-row.  Rewritten
--      probability must match the baseline.
CREATE TABLE pd_mc_a(x int);
CREATE TABLE pd_mc_b(y int);
INSERT INTO pd_mc_a VALUES (1), (1), (2);
INSERT INTO pd_mc_b VALUES (10), (10), (20);
SELECT add_provenance('pd_mc_a');
SELECT add_provenance('pd_mc_b');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pd_mc_a;
  PERFORM set_prob(provsql, 0.4) FROM pd_mc_b;
END $$;

SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base13 AS
  SELECT a.x, b.y, probability_evaluate(provenance()) AS p
    FROM pd_mc_a a, pd_mc_b b
   GROUP BY a.x, b.y;
SELECT remove_provenance('pd_base13');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew13 AS
  SELECT a.x, b.y, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_mc_a a, pd_mc_b b
   GROUP BY a.x, b.y;
SELECT remove_provenance('pd_rew13');

SELECT b.x, b.y, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base13 b JOIN pd_rew13 r ON b.x = r.x AND b.y = r.y
 ORDER BY b.x, b.y;

-- (14) Multi-component + all-constant targetList (Boolean-existence
--      idiom over disconnected components):
--        SELECT DISTINCT 1 FROM A, B
--      where A and B share no variable.  No user TE carries an atom
--      Var; each component's inner sub-Query gets a synthetic
--      Const(1) anchor so it still produces one row per "grouping"
--      (here, one row total).  The outer Cartesian-products the two
--      one-row inners and the user's DISTINCT keeps the single
--      output row.  Rewritten probability must match the baseline
--      (P(non-empty join) = (1 - prod(1 - p_a)) * (1 - prod(1 - p_b))).
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base14 AS
  SELECT DISTINCT 1 AS one FROM pd_mc_a a, pd_mc_b b;
CREATE TEMP TABLE pd_base14_p AS
  SELECT one, ROUND(probability_evaluate(provsql)::numeric, 6) AS p
    FROM pd_base14;
SELECT remove_provenance('pd_base14_p');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew14 AS
  SELECT DISTINCT 1 AS one FROM pd_mc_a a, pd_mc_b b;
CREATE TEMP TABLE pd_rew14_p AS
  SELECT one, ROUND(probability_evaluate(provsql, 'independent')::numeric, 6) AS p
    FROM pd_rew14;
SELECT remove_provenance('pd_rew14_p');

SELECT b.one, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base14_p b JOIN pd_rew14_p r ON b.one = r.one;

-- (15) Multi-component asymmetric: one component carries a Var in the
--      user's targetList (a.x), the other one does not.  Per output
--      row x = x_t:
--        P(x_t) = P(A has x_t) * P(B is non-empty).
--      pd_mc_b's inner sub-Query gets a synthetic Const(1) anchor.
SET provsql.boolean_provenance = off;
CREATE TEMP TABLE pd_base15 AS
  SELECT a.x, probability_evaluate(provenance()) AS p
    FROM pd_mc_a a, pd_mc_b b
   GROUP BY a.x;
SELECT remove_provenance('pd_base15');

SET provsql.boolean_provenance = on;
CREATE TEMP TABLE pd_rew15 AS
  SELECT a.x, probability_evaluate(provenance(), 'independent') AS p
    FROM pd_mc_a a, pd_mc_b b
   GROUP BY a.x;
SELECT remove_provenance('pd_rew15');

SELECT b.x, ROUND((b.p - r.p)::numeric, 9) AS diff_baseline_vs_rewritten
  FROM pd_base15 b JOIN pd_rew15 r ON b.x = r.x
 ORDER BY b.x;

-- (6) Non-hierarchical CQ must bail.  Classes X = {a.x, c.x},
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
DROP TABLE pd_a, pd_b, pd_c, pd_g, pd_p, pd_q, pd_r,
           pd_n_a, pd_n_b, pd_n_c, pd_n_d,
           pd_dj_a, pd_dj_b, pd_dj_c, pd_dj_d,
           pd_bid_a, pd_bid_b, pd_bid_p, pd_bid_empty,
           pd_mc_a, pd_mc_b,
           pd_d, pd_e, pd_f;
