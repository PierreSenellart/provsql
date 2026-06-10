\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql,public;

-- The joint-width UCQ compiler (ucq_joint_evaluate / ucq_joint_compile_stats)
-- evaluates an arbitrary union of conjunctive queries -- including the
-- queries that are #P-hard under the Dalvi-Suciu dichotomy (H0 =
-- R(x),S(x,y),T(y) and the Hk family) -- exactly, in time tractable
-- whenever the joint treewidth of the data and its correlation structure
-- is bounded (the data-graph regime here: every fact an independent
-- gate_input, so the joint graph is the Gaifman graph of the facts and
-- the screen is the data treewidth).  A UCQ-specialised homomorphism-type
-- DP runs over a tree decomposition of the joint graph, emitting a
-- certified d-D by construction; these columnar / JSON forms are
-- exercised here against closed-form probabilities.

-- Convenience: a UUID per name.
CREATE OR REPLACE FUNCTION u(name text) RETURNS uuid AS $$
  SELECT public.uuid_generate_v5(uuid_ns_provsql(), name);
$$ LANGUAGE sql IMMUTABLE;

-- H0 = R(x), S(x,y), T(y) over two disjoint chains 0-2, 1-3, all events
-- 0.5.  Closed form: 1 - (1 - 0.5^3)^2 = 0.234375.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":4,"atoms":[
      {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0, 1,1, 2,2],
  ARRAY[0, 1, 0,2, 1,3, 2, 3],
  ARRAY[1,1, 2,2, 1,1],
  ARRAY[u('r0'),u('r1'),u('s02'),u('s13'),u('t2'),u('t3')]::uuid[],
  ARRAY[0.5,0.5,0.5,0.5,0.5,0.5])::numeric, 6) AS h0_two_chains;

-- H0 with a shared y (correlation through the query, not the data):
-- r0=0.7, r1=0.3, s02=0.6, s12=0.4, t2=0.8.  y=2 only:
-- 0.8 * (1 - (1-0.7*0.6)(1-0.3*0.4)) = 0.39168.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":3,"atoms":[
      {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0, 1,1, 2],
  ARRAY[0, 1, 0,2, 1,2, 2],
  ARRAY[1,1, 2,2, 1],
  ARRAY[u('r0'),u('r1'),u('s02'),u('s12'),u('t2')]::uuid[],
  ARRAY[0.7,0.3,0.6,0.4,0.8])::numeric, 6) AS h0_shared_y;

-- Prop 4.2.11(a): chi = OR_i (A(a_i,b_i) AND B(b_i,c_i)) with disjoint
-- variables per conjunct -> joint width O(1).  a0=0.3,b0=0.4 (witness
-- 0,1,2), a1=0.5,b1=0.6 (witness 3,4,5).
-- 1 - (1 - 0.3*0.4)(1 - 0.5*0.6) = 0.384.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":3,"atoms":[
      {"rel":0,"vars":[0,1]},{"rel":1,"vars":[1,2]}]}]}'::jsonb,
  ARRAY[0,1, 0,1],
  ARRAY[0,1, 1,2, 3,4, 4,5],
  ARRAY[2,2, 2,2],
  ARRAY[u('a0'),u('b0'),u('a1'),u('b1')]::uuid[],
  ARRAY[0.3,0.4,0.5,0.6])::numeric, 6) AS prop_4_2_11_a;

-- Prop 4.2.11(c): a 6-cycle over S, query exists x,y S(x,y), all 0.5 ->
-- 1 - (1-0.5)^6 = 0.984375; accepted with joint treewidth 2.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0,1]}]}]}'::jsonb,
  ARRAY[0,0,0,0,0,0],
  ARRAY[0,1, 1,2, 2,3, 3,4, 4,5, 5,0],
  ARRAY[2,2,2,2,2,2],
  ARRAY[u('c0'),u('c1'),u('c2'),u('c3'),u('c4'),u('c5')]::uuid[],
  ARRAY[0.5,0.5,0.5,0.5,0.5,0.5])::numeric, 6) AS prop_4_2_11_c_cycle;

-- A self-join R(x), S(x,y), R(y) over a path 0-1-2, all 0.5.
-- Satisfied iff some edge (x,y) has both endpoints in R: edges (0,1),(1,2).
-- P = 1 - (1 - r0 r1 s01)(1 - r1 r2 s12) with overlap on r1:
-- worlds: enumerate -> 0.21875.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":2,"atoms":[
      {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":0,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0,0, 1,1],
  ARRAY[0, 1, 2, 0,1, 1,2],
  ARRAY[1,1,1, 2,2],
  ARRAY[u('r0'),u('r1'),u('r2'),u('s01'),u('s12')]::uuid[],
  ARRAY[0.5,0.5,0.5,0.5,0.5])::numeric, 6) AS self_join_R_S_R;

-- UCQ with two disjuncts: H0 over 0-1, OR exists x R(x).  R has r0,r2;
-- the second disjunct alone gives 1-(1-0.5)^2 = 0.75 over {r0,r2}; the
-- first adds nothing new (R(0) already in the OR) -> 0.75.
SELECT round(ucq_joint_evaluate(
  '{"disjuncts":[
      {"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]},
      {"n_vars":1,"atoms":[{"rel":0,"vars":[0]}]}]}'::jsonb,
  ARRAY[0,1,2, 0],
  ARRAY[0, 0,1, 1, 2],
  ARRAY[1,2,1, 1],
  ARRAY[u('r0'),u('s01'),u('t1'),u('r2')]::uuid[],
  ARRAY[0.5,0.5,0.5,0.5])::numeric, 6) AS ucq_two_disjuncts;

-- Stats: the three width columns on the two-chain H0 instance.
SELECT joint_treewidth, data_treewidth_lb, circuit_treewidth_lb
FROM ucq_joint_compile_stats(
  '{"disjuncts":[{"n_vars":4,"atoms":[
      {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0, 1,1, 2,2],
  ARRAY[0, 1, 0,2, 1,3, 2, 3],
  ARRAY[1,1, 2,2, 1,1],
  ARRAY[u('r0'),u('r1'),u('s02'),u('s13'),u('t2'),u('t3')]::uuid[],
  ARRAY[0.5,0.5,0.5,0.5,0.5,0.5]);

-- Prop 4.2.11(b): a high-joint-width instance must be rejected (the
-- caller then falls back to the ladder).  With the joint-width cap
-- lowered to 1, a triangle over S (Gaifman treewidth 2) is rejected by
-- the degeneracy screen.
SET provsql.joint_max_treewidth = 1;
SELECT ucq_joint_evaluate(
  '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0,1]}]}]}'::jsonb,
  ARRAY[0,0,0],
  ARRAY[0,1, 1,2, 0,2],
  ARRAY[2,2,2],
  ARRAY[u('e01'),u('e12'),u('e02')]::uuid[],
  ARRAY[0.5,0.5,0.5]) AS should_error;
RESET provsql.joint_max_treewidth;

DROP FUNCTION u(text);
