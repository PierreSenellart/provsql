\set ECHO none
\pset format unaligned

-- Safe-UCQ Möbius-inversion route (mobius_evaluate.cpp): the last missing
-- exact route of the Dalvi-Suciu dichotomy.  Some unions of conjunctive
-- queries are safe (PTIME data complexity) only because the #P-hard terms of
-- their inclusion-exclusion expansion carry a zero Möbius value on the CNF
-- lattice and cancel.  The query-side analysis builds the CNF lattice over the
-- disjuncts' connected components, collapses elements up to logical
-- equivalence (homomorphism), computes the coefficients, recurses by the
-- IndepStep / MobiusStep lifted-inference rules (component split, separator
-- independent-project, inner Möbius step), and materialises a gate_mobius-
-- rooted circuit (a signed combination over certified-independent islands)
-- answered in PTIME by the standard probability path.
--
-- This file drives the descriptor entry points directly over real
-- tuple-independent tables (mirroring ucq_joint.sql), cross-checking the
-- probabilities and the lattice statistics.  v1: reduced form, TID inputs.

SET provsql.provenance TO 'boolean';

-- ===========================================================================
-- Warm-up: Phi1 = R(x),S(x,y) v S(x,y),T(y) v R(x),T(y).
--
-- Safe, non-hierarchical.  Its CNF is (gR v gST) ∧ (gT v gRS) and the lattice
-- bottom collapses by implication to gR v gT -- safe, so this exercises the
-- CNF conversion, the equivalence collapsing and the minimisation WITHOUT a
-- zero-Möbius cancellation (n_cancelled = 0).  Brute force over the 8 tuples
-- gives 0.855190.
-- ===========================================================================
CREATE TABLE mob_phi_r(x int);
INSERT INTO mob_phi_r VALUES (1),(2);
SELECT add_provenance('mob_phi_r');
CREATE TABLE mob_phi_s(x int, y int);
INSERT INTO mob_phi_s VALUES (1,1),(1,2),(2,2);
SELECT add_provenance('mob_phi_s');
CREATE TABLE mob_phi_t(y int);
INSERT INTO mob_phi_t VALUES (1),(2);
SELECT add_provenance('mob_phi_t');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.6) FROM mob_phi_r WHERE x=1;
  PERFORM set_prob(provsql, 0.4) FROM mob_phi_r WHERE x=2;
  PERFORM set_prob(provsql, 0.5) FROM mob_phi_s WHERE x=1 AND y=1;
  PERFORM set_prob(provsql, 0.7) FROM mob_phi_s WHERE x=1 AND y=2;
  PERFORM set_prob(provsql, 0.3) FROM mob_phi_s WHERE x=2 AND y=2;
  PERFORM set_prob(provsql, 0.5) FROM mob_phi_t WHERE y=1;
  PERFORM set_prob(provsql, 0.55) FROM mob_phi_t WHERE y=2;
END $$;

\set phi_desc '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]}]},{"n_vars":2,"atoms":[{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]},{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":2,"vars":[1]}]}],"relations":["provsql_test.mob_phi_r","provsql_test.mob_phi_s","provsql_test.mob_phi_t"],"elem_cols":[["x"],["x","y"],["y"]]}'

SELECT 'phi1' AS q,
       round(probability::numeric,6) AS probability,
       n_components, n_cnf_conjuncts, lattice_size, n_nonzero,
       n_cancelled, cancelled_hard
  FROM provsql.mobius_compile_stats(:'phi_desc'::jsonb);

-- The materialised token evaluates to the same probability through the
-- standard probability path.
SELECT 'phi1_token' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'phi_desc'::jsonb))::numeric,6)
         AS probability;

-- ===========================================================================
-- Flagship: q9 / QW over R, S1, S2, S3, T (Dalvi-Suciu; q9 in Monet &
-- Olteanu).  Its C-lattice has a 4-component literal set {h0,h1,h2,h3}; the
-- bottom H3 = h0 v h1 v h2 v h3 is #P-hard and carries Möbius value zero, so it
-- cancels and never needs evaluating.  Brute force over the 13 tuples gives
-- 0.574136 -- and the stats show exactly one cancelled, #P-hard element: the
-- single number that makes the mechanism legible.
-- ===========================================================================
CREATE TABLE mob_r(x int);
INSERT INTO mob_r VALUES (1),(2);
SELECT add_provenance('mob_r');
CREATE TABLE mob_s1(x int, y int);
INSERT INTO mob_s1 VALUES (1,1),(1,2),(2,1);
SELECT add_provenance('mob_s1');
CREATE TABLE mob_s2(x int, y int);
INSERT INTO mob_s2 VALUES (1,1),(2,2),(1,2);
SELECT add_provenance('mob_s2');
CREATE TABLE mob_s3(x int, y int);
INSERT INTO mob_s3 VALUES (1,1),(2,2),(1,2);
SELECT add_provenance('mob_s3');
CREATE TABLE mob_t(y int);
INSERT INTO mob_t VALUES (1),(2);
SELECT add_provenance('mob_t');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.6) FROM mob_r WHERE x=1;
  PERFORM set_prob(provsql, 0.4) FROM mob_r WHERE x=2;
  PERFORM set_prob(provsql, 0.5) FROM mob_s1 WHERE x=1 AND y=1;
  PERFORM set_prob(provsql, 0.7) FROM mob_s1 WHERE x=1 AND y=2;
  PERFORM set_prob(provsql, 0.3) FROM mob_s1 WHERE x=2 AND y=1;
  PERFORM set_prob(provsql, 0.45) FROM mob_s2 WHERE x=1 AND y=1;
  PERFORM set_prob(provsql, 0.55) FROM mob_s2 WHERE x=2 AND y=2;
  PERFORM set_prob(provsql, 0.5) FROM mob_s2 WHERE x=1 AND y=2;
  PERFORM set_prob(provsql, 0.65) FROM mob_s3 WHERE x=1 AND y=1;
  PERFORM set_prob(provsql, 0.35) FROM mob_s3 WHERE x=2 AND y=2;
  PERFORM set_prob(provsql, 0.4) FROM mob_s3 WHERE x=1 AND y=2;
  PERFORM set_prob(provsql, 0.5) FROM mob_t WHERE y=1;
  PERFORM set_prob(provsql, 0.6) FROM mob_t WHERE y=2;
END $$;

-- q9 as a UCQ (the 4 prime implicants of the CNF, each h_i = its two atoms;
-- per-disjunct existential variables are independent across disjuncts):
--   D0 = h0,h3   D1 = h1,h3   D2 = h2,h3   D3 = h0,h1,h2
\set q9_desc '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":3,"vars":[2,3]},{"rel":4,"vars":[3]}]},{"n_vars":4,"atoms":[{"rel":1,"vars":[0,1]},{"rel":2,"vars":[0,1]},{"rel":3,"vars":[2,3]},{"rel":4,"vars":[3]}]},{"n_vars":4,"atoms":[{"rel":2,"vars":[0,1]},{"rel":3,"vars":[0,1]},{"rel":3,"vars":[2,3]},{"rel":4,"vars":[3]}]},{"n_vars":6,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":1,"vars":[2,3]},{"rel":2,"vars":[2,3]},{"rel":2,"vars":[4,5]},{"rel":3,"vars":[4,5]}]}],"relations":["provsql_test.mob_r","provsql_test.mob_s1","provsql_test.mob_s2","provsql_test.mob_s3","provsql_test.mob_t"],"elem_cols":[["x"],["x","y"],["x","y"],["x","y"],["y"]]}'

SELECT 'q9' AS q,
       round(probability::numeric,6) AS probability,
       n_components, n_cancelled, cancelled_hard
  FROM provsql.mobius_compile_stats(:'q9_desc'::jsonb);

SELECT 'q9_token' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb))::numeric,6)
         AS probability;

-- The 'mobius' method is a first-class, by-name-invocable catalog method
-- (modelled on 'inversion-free', not a terminal special-case): naming it
-- explicitly gives the same exact answer as the default chain.
SELECT 'q9_named' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb), 'mobius')::numeric,6)
         AS probability;

-- Granted-tolerance paths ('relative' / 'additive') must take the fast exact
-- Möbius route too: it is exact and linear, so it trivially meets any tolerance
-- ("exact when cheap").  Before, they fell through to an FPRAS on the #P-hard
-- literal lineage and could time out; both must now equal the default.
SELECT 'q9_tolerance' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb),
               'relative', 'eps=0.1,delta=0.05')::numeric,6)
         = round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb))::numeric,6)
         AS relative_exact,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb),
               'additive', 'eps=0.1,delta=0.05')::numeric,6)
         = round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'q9_desc'::jsonb))::numeric,6)
         AS additive_exact;

-- Planner auto-routing ("just works"): q9 written as a plain SQL UNION (the
-- existence formed by the UNION dedup), with the joint-width width screen
-- forced to decline (joint_max_treewidth = 0) so the runtime hands off to the
-- Möbius route -- exactly the adversarial-data situation where the joint
-- treewidth is unbounded and Möbius is the only exact route.  The planner hook
-- recognises the UCQ, the Möbius compiler fires, and the per-row token is a
-- gate_mobius answered in PTIME -- no manual intervention.
SET provsql.active = on;
SET provsql.joint_width = on;
SET provsql.mobius = on;
SET provsql.joint_max_treewidth = 0;     -- force the joint screen to decline -> Möbius
CREATE TEMP TABLE q9auto AS
  SELECT provenance() AS tok FROM (
    SELECT 1 FROM mob_r, mob_s1 a1, mob_s3 a3, mob_t t3
      WHERE mob_r.x=a1.x AND a3.y=t3.y
    UNION
    SELECT 1 FROM mob_s1 b1, mob_s2 b2, mob_s3 b3, mob_t tb
      WHERE b1.x=b2.x AND b1.y=b2.y AND b3.y=tb.y
    UNION
    SELECT 1 FROM mob_s2 c2, mob_s3 c3, mob_s3 c3b, mob_t tc
      WHERE c2.x=c3.x AND c2.y=c3.y AND c3b.y=tc.y
    UNION
    SELECT 1 FROM mob_r d, mob_s1 d1, mob_s1 d1b, mob_s2 d2, mob_s2 d2b, mob_s3 d3
      WHERE d.x=d1.x AND d1b.x=d2.x AND d1b.y=d2.y AND d2b.x=d3.x AND d2b.y=d3.y
  ) qq;
-- The literal lineage of the same query (the normal provenance), to cross-check
-- that the Möbius token preserves every non-probability evaluation.
SET provsql.mobius = off;
SET provsql.joint_width = off;
CREATE TEMP TABLE q9lit AS
  SELECT provenance() AS tok FROM (
    SELECT 1 FROM mob_r, mob_s1 a1, mob_s3 a3, mob_t t3
      WHERE mob_r.x=a1.x AND a3.y=t3.y
    UNION
    SELECT 1 FROM mob_s1 b1, mob_s2 b2, mob_s3 b3, mob_t tb
      WHERE b1.x=b2.x AND b1.y=b2.y AND b3.y=tb.y
    UNION
    SELECT 1 FROM mob_s2 c2, mob_s3 c3, mob_s3 c3b, mob_t tc
      WHERE c2.x=c3.x AND c2.y=c3.y AND c3b.y=tc.y
    UNION
    SELECT 1 FROM mob_r d, mob_s1 d1, mob_s1 d1b, mob_s2 d2, mob_s2 d2b, mob_s3 d3
      WHERE d.x=d1.x AND d1b.x=d2.x AND d1b.y=d2.y AND d2b.x=d3.x AND d2b.y=d3.y
  ) qq;
SET provsql.active = off;
SET provsql.joint_max_treewidth = 10;
SET provsql.mobius = on;
SET provsql.joint_width = on;
SELECT 'q9_planner' AS q,
       provsql.get_gate_type(tok) AS root_gate,
       round(provsql.probability_evaluate(tok)::numeric,6) AS probability
  FROM q9auto;

-- The Möbius token is the inversion-free model, not a measure-only gate: it
-- carries the literal lineage, so the default route uses the fast Möbius
-- combination while every OTHER evaluation (a named probability method,
-- Shapley, ...) passes through to the normal provenance and matches evaluating
-- that lineage directly.
--
-- Shapley over q9's #P-hard lineage needs a knowledge compiler (its treewidth
-- exceeds the in-process cap), so on a runner with none installed the Shapley
-- arm is skipped (it is exercised wherever a compiler is present); a genuine
-- mismatch or any other error still fails.
CREATE FUNCTION mob_shapley_matches(a UUID, b UUID, v UUID) RETURNS boolean AS $$
BEGIN
  RETURN round(provsql.shapley(a, v)::numeric, 6)
       = round(provsql.shapley(b, v)::numeric, 6);
EXCEPTION WHEN OTHERS THEN
  IF SQLERRM LIKE '%no knowledge compiler%' THEN RETURN true; END IF;
  RAISE;
END;
$$ LANGUAGE plpgsql;

SELECT 'mobius_keeps_evaluations' AS q,
       round(provsql.probability_evaluate(m.tok)::numeric,6)
         = round(provsql.probability_evaluate(m.tok,'possible-worlds')::numeric,6)
         AS pw_matches_default,
       round(provsql.probability_evaluate(m.tok,'possible-worlds')::numeric,6)
         = round(provsql.probability_evaluate(l.tok,'possible-worlds')::numeric,6)
         AS pw_matches_lineage,
       mob_shapley_matches(m.tok, l.tok, (SELECT provsql FROM mob_r WHERE x=1))
         AS shapley_matches_lineage
  FROM q9auto m, q9lit l;

-- ===========================================================================
-- Per-answer (free head variable): grouped Φ₁ over R(g,x), S(g,x,y), T(g,y),
-- the head g pinned per output group.  ucq_mobius_provenance_answer gathers the
-- facts once, then for each group binds the head variable (canonical index 0)
-- to the group's value and compiles a head-pinned Möbius circuit.  Two groups
-- with different data; brute force gives 0.855190 and 0.858256.
-- ===========================================================================
CREATE TABLE mob_gr(g int, x int);
INSERT INTO mob_gr VALUES (1,1),(1,2),(2,1),(2,2);
SELECT add_provenance('mob_gr');
CREATE TABLE mob_gs(g int, x int, y int);
INSERT INTO mob_gs VALUES (1,1,1),(1,1,2),(1,2,2),(2,1,1),(2,2,1);
SELECT add_provenance('mob_gs');
CREATE TABLE mob_gt(g int, y int);
INSERT INTO mob_gt VALUES (1,1),(1,2),(2,1),(2,2);
SELECT add_provenance('mob_gt');
DO $$ BEGIN
  PERFORM set_prob(provsql,0.6) FROM mob_gr WHERE g=1 AND x=1;
  PERFORM set_prob(provsql,0.4) FROM mob_gr WHERE g=1 AND x=2;
  PERFORM set_prob(provsql,0.3) FROM mob_gr WHERE g=2 AND x=1;
  PERFORM set_prob(provsql,0.8) FROM mob_gr WHERE g=2 AND x=2;
  PERFORM set_prob(provsql,0.5) FROM mob_gs WHERE g=1 AND x=1 AND y=1;
  PERFORM set_prob(provsql,0.7) FROM mob_gs WHERE g=1 AND x=1 AND y=2;
  PERFORM set_prob(provsql,0.3) FROM mob_gs WHERE g=1 AND x=2 AND y=2;
  PERFORM set_prob(provsql,0.4) FROM mob_gs WHERE g=2 AND x=1 AND y=1;
  PERFORM set_prob(provsql,0.6) FROM mob_gs WHERE g=2 AND x=2 AND y=1;
  PERFORM set_prob(provsql,0.5) FROM mob_gt WHERE g=1 AND y=1;
  PERFORM set_prob(provsql,0.55) FROM mob_gt WHERE g=1 AND y=2;
  PERFORM set_prob(provsql,0.7) FROM mob_gt WHERE g=2 AND y=1;
  PERFORM set_prob(provsql,0.2) FROM mob_gt WHERE g=2 AND y=2;
END $$;

\set ga_desc '{"disjuncts":[{"n_vars":3,"atoms":[{"rel":0,"vars":[0,1]},{"rel":1,"vars":[0,1,2]}]},{"n_vars":3,"atoms":[{"rel":1,"vars":[0,1,2]},{"rel":2,"vars":[0,2]}]},{"n_vars":3,"atoms":[{"rel":0,"vars":[0,1]},{"rel":2,"vars":[0,2]}]}],"relations":["provsql_test.mob_gr","provsql_test.mob_gs","provsql_test.mob_gt"],"elem_cols":[["g","x"],["g","x","y"],["g","y"]]}'

SELECT 'per_answer' AS q, v.g AS head_g,
       round(provsql.probability_evaluate(
         provsql.ucq_mobius_provenance_answer(
           :'ga_desc'::jsonb, ARRAY[0], ARRAY[v.g]))::numeric,6) AS probability
  FROM (VALUES ('1'),('2')) v(g)
  ORDER BY v.g;

-- ===========================================================================
-- Negative: a non-hierarchical single CQ H0 = R(x),S(x,y),T(y) has no
-- separator and no inclusion-exclusion structure -- it is genuinely #P-hard,
-- not safe.  The Möbius route declines (it is not in the supported class), so
-- ucq_mobius_provenance returns the fallback (here NULL) and the query would
-- fall through to the generic pipeline.
-- ===========================================================================
\set h0_desc '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}],"relations":["provsql_test.mob_phi_r","provsql_test.mob_phi_s","provsql_test.mob_phi_t"],"elem_cols":[["x"],["x","y"],["y"]]}'

SELECT 'h0_declines' AS q,
       provsql.ucq_mobius_provenance(:'h0_desc'::jsonb) IS NULL AS declined;
