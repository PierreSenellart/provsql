\set ECHO none
\pset format unaligned

-- Self-joins on the safe-UCQ Möbius-inversion route (mobius_evaluate.cpp).
--
-- A UCQ whose reduced form still repeats a relation symbol is handled by the
-- two normalizations the Dalvi-Suciu dichotomy runs before its lifted
-- recursion -- ranking (Def. 4.1) and shattering (Prop. 2.10) -- plus the
-- disjunctive detour a genuine self-join forces:
--
--   * ranking / shattering (one shard split in normalizeShards) partitions a
--     relation whose atoms carry different patterns -- a pinned constant, a
--     repeated variable -- into disjoint shard symbols, restoring the reduced
--     form the independence certificates need;
--   * two components over ONE shard symbol (the JACM's q_J) are not
--     independent, so instead of an independent product the compiler takes the
--     Möbius step: P(c1 ∧ c2) = P(c1) + P(c2) - P(c1 ∨ c2), Example 3.1 of the
--     paper.
--
-- Every probability below is cross-checked against 'possible-worlds' on the
-- literal lineage of the same query -- the brute-force enumeration -- so the
-- file certifies the answers rather than pinning remembered constants.

SET provsql.provenance TO 'boolean';

CREATE TABLE mobsj_r(x int);
INSERT INTO mobsj_r VALUES (1),(2);
SELECT add_provenance('mobsj_r');
CREATE TABLE mobsj_s(x int, y int);
INSERT INTO mobsj_s VALUES (1,1),(1,2),(2,2),(2,3);
SELECT add_provenance('mobsj_s');
CREATE TABLE mobsj_t(y int);
INSERT INTO mobsj_t VALUES (1),(2);
SELECT add_provenance('mobsj_t');
CREATE TABLE mobsj_w(x int);
INSERT INTO mobsj_w VALUES (1),(2);
SELECT add_provenance('mobsj_w');
DO $$ BEGIN
  PERFORM set_prob(provsql,0.35) FROM mobsj_r WHERE x=1;
  PERFORM set_prob(provsql,0.65) FROM mobsj_r WHERE x=2;
  PERFORM set_prob(provsql,0.6) FROM mobsj_s WHERE x=1 AND y=1;
  PERFORM set_prob(provsql,0.4) FROM mobsj_s WHERE x=1 AND y=2;
  PERFORM set_prob(provsql,0.5) FROM mobsj_s WHERE x=2 AND y=2;
  PERFORM set_prob(provsql,0.7) FROM mobsj_s WHERE x=2 AND y=3;
  PERFORM set_prob(provsql,0.55) FROM mobsj_t WHERE y=1;
  PERFORM set_prob(provsql,0.45) FROM mobsj_t WHERE y=2;
  PERFORM set_prob(provsql,0.5) FROM mobsj_w WHERE x=1;
  PERFORM set_prob(provsql,0.3) FROM mobsj_w WHERE x=2;
END $$;

-- ===========================================================================
-- 1. q_J = R(x1),S(x1,y1),W(x2),S(x2,y2): one disjunct, two variable-connected
--    components sharing S.  Not an independent product; the Möbius step turns
--    it into P(c1) + P(c2) - P(c1 ∨ c2), and each term compiles by the ordinary
--    separator / component rules.  (Before, this was the decline "within-
--    disjunct self-join -- needs ranking/shattering".)
-- ===========================================================================
\set qj_desc '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":3,"vars":[2]},{"rel":1,"vars":[2,3]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'

SELECT 'q_J' AS q,
       round(probability::numeric,6) AS probability,
       n_components, n_cnf_conjuncts, n_nonzero, n_cancelled
  FROM provsql.mobius_compile_stats(:'qj_desc'::jsonb);

-- The materialised token, and the brute-force reference on the same query.
SET provsql.active = on;
SET provsql.mobius = off;
SET provsql.joint_width = off;
CREATE TEMP TABLE qj_lit AS
  SELECT provenance() AS tok FROM (
    SELECT DISTINCT 1 FROM mobsj_r r, mobsj_s s1, mobsj_w w, mobsj_s s2
      WHERE r.x=s1.x AND w.x=s2.x) qq;
SET provsql.active = off;

SELECT 'q_J_token' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'qj_desc'::jsonb))::numeric,6)
         = round(provsql.probability_evaluate(tok,'possible-worlds')::numeric,6)
         AS matches_brute_force
  FROM qj_lit;

-- Planner auto-routing: the same query written as plain SQL (a self-join over
-- mobsj_s), recognised at planning time and rooted in a gate_mobius.
SET provsql.active = on;
SET provsql.mobius = on;
SET provsql.joint_width = on;
CREATE TEMP TABLE qj_auto AS
  SELECT provenance() AS tok FROM (
    SELECT DISTINCT 1 FROM mobsj_r r, mobsj_s s1, mobsj_w w, mobsj_s s2
      WHERE r.x=s1.x AND w.x=s2.x) qq;
SET provsql.active = off;

SELECT 'q_J_planner' AS q,
       provsql.get_gate_type(a.tok) AS root_gate,
       round(provsql.probability_evaluate(a.tok)::numeric,6)
         = round(provsql.probability_evaluate(l.tok,'possible-worlds')::numeric,6)
         AS matches_brute_force
  FROM qj_auto a, qj_lit l;

-- Every non-probability evaluation still runs on the literal lineage carried
-- by the gate_mobius, so a named method answers exactly as on the ordinary
-- provenance.
SELECT 'q_J_lineage_kept' AS q,
       round(provsql.probability_evaluate(a.tok,'possible-worlds')::numeric,6)
         = round(provsql.probability_evaluate(l.tok,'possible-worlds')::numeric,6)
         AS pw_matches_lineage
  FROM qj_auto a, qj_lit l;

-- ===========================================================================
-- 2. Shattering only: a head-pinned self-join.  Q(g) :- S(g,y),T(y),S(u,v),R(u)
--    pins the head in ONE of the two S atoms, so per output group the sentence
--    mixes a ground and a variable slot on one relation -- overlapping tuple
--    sets that shattering separates into the shards S_{x=g} / S_{x≠g}.
-- ===========================================================================
\set sh_desc '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]},{"rel":1,"vars":[2,3]},{"rel":0,"vars":[2]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'

SET provsql.active = on;
SET provsql.mobius = off;
SET provsql.joint_width = off;
CREATE TEMP TABLE sh_lit AS
  SELECT g, provenance() AS tok FROM (
    SELECT DISTINCT s1.x AS g FROM mobsj_s s1, mobsj_t t, mobsj_s s2, mobsj_r r
      WHERE s1.y=t.y AND s2.x=r.x) qq;
SET provsql.active = off;

SELECT 'shattered_answer' AS q, l.g,
       round(provsql.probability_evaluate(
         provsql.ucq_mobius_provenance_answer(
           :'sh_desc'::jsonb, ARRAY[0], ARRAY[l.g::text]))::numeric,6)
         = round(provsql.probability_evaluate(l.tok,'possible-worlds')::numeric,6)
         AS matches_brute_force
  FROM sh_lit l
  ORDER BY l.g;

-- ===========================================================================
-- 3. Ranking only: S(x,x) and S(u,v) over one relation.  The atoms have
--    different equality patterns, so their tuple sets overlap; ranking splits S
--    on the equality pattern of its tuples (at most Bell(k) shards) and the two
--    atoms land on disjoint symbols.
-- ===========================================================================
\set rk_desc '{"disjuncts":[{"n_vars":3,"atoms":[{"rel":1,"vars":[0,0]},{"rel":2,"vars":[0]},{"rel":1,"vars":[1,2]},{"rel":0,"vars":[1]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'

SET provsql.active = on;
SET provsql.mobius = off;
SET provsql.joint_width = off;
CREATE TEMP TABLE rk_lit AS
  SELECT provenance() AS tok FROM (
    SELECT DISTINCT 1 FROM mobsj_s s1, mobsj_t t, mobsj_s s2, mobsj_r r
      WHERE s1.x=s1.y AND t.y=s1.x AND s2.x=r.x) qq;
SET provsql.active = off;

SELECT 'ranked' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'rk_desc'::jsonb))::numeric,6)
         = round(provsql.probability_evaluate(tok,'possible-worlds')::numeric,6)
         AS matches_brute_force
  FROM rk_lit;

-- ===========================================================================
-- 4. The separator is one variable per disjunct.  In
--    (∃x R(x) ∧ ∃y T(y)) ∨ ∃z (R(z) ∧ T(z)) the (rel,pos) unification puts x, y
--    and z in one class covering every atom, but x and y are TWO variables of
--    the first disjunct: substituting the same constant for both would compute
--    ⋁_a R(a)∧T(a), a strictly stronger query.  The class is refused, the
--    sentence goes to the Möbius step, and the answer is the true one.
-- ===========================================================================
\set two_desc '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":2,"vars":[1]}]},{"n_vars":1,"atoms":[{"rel":0,"vars":[0]},{"rel":2,"vars":[0]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'

SET provsql.active = on;
SET provsql.mobius = off;
SET provsql.joint_width = off;
CREATE TEMP TABLE two_lit AS
  SELECT provenance() AS tok FROM (
    SELECT DISTINCT 1 FROM mobsj_r r, mobsj_t t
    UNION
    SELECT DISTINCT 1 FROM mobsj_r r2, mobsj_t t2 WHERE r2.x=t2.y) qq;
SET provsql.active = off;

SELECT 'two_class_vars' AS q,
       round(provsql.probability_evaluate(
               provsql.ucq_mobius_provenance(:'two_desc'::jsonb))::numeric,6)
         = round(provsql.probability_evaluate(tok,'possible-worlds')::numeric,6)
         AS matches_brute_force
  FROM two_lit;

-- ===========================================================================
-- 5. Must-decline shapes.  A self-join carrying an inversion is not even in
--    UCQ(OBDD): S(x,y),S(y,x) and S(x,y),S(y,z) have a covering class with two
--    variables per disjunct, no decomposition, and no inclusion-exclusion
--    structure.  They must decline (the query then falls back to the ordinary
--    provenance), never answer wrongly.
-- ===========================================================================
\set sym_desc '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":1,"vars":[0,1]},{"rel":1,"vars":[1,0]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'
\set path_desc '{"disjuncts":[{"n_vars":3,"atoms":[{"rel":1,"vars":[0,1]},{"rel":1,"vars":[1,2]}]}],"relations":["provsql_test.mobsj_r","provsql_test.mobsj_s","provsql_test.mobsj_t","provsql_test.mobsj_w"],"elem_cols":[["x"],["x","y"],["y"],["x"]]}'

SELECT 'inversion_declines' AS q,
       provsql.ucq_mobius_provenance(:'sym_desc'::jsonb) IS NULL AS symmetric,
       provsql.ucq_mobius_provenance(:'path_desc'::jsonb) IS NULL AS two_hop;

-- ===========================================================================
-- 6. provsql.mobius_max_cnf caps the inclusion-exclusion lattice (2^M elements
--    for M conjuncts).  Below the CNF of q_J the route declines cleanly.
-- ===========================================================================
SET provsql.mobius_max_cnf = 1;
SELECT 'cnf_cap' AS q,
       provsql.ucq_mobius_provenance(:'qj_desc'::jsonb) IS NULL AS declined;
RESET provsql.mobius_max_cnf;
SELECT 'cnf_cap_reset' AS q,
       provsql.ucq_mobius_provenance(:'qj_desc'::jsonb) IS NOT NULL AS compiles;

RESET provsql.mobius;
RESET provsql.joint_width;
