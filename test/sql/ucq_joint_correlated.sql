\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql,public;
SET provsql.provenance = 'boolean';

-- The correlated regime of the joint-width UCQ compiler
-- (ucq_joint_evaluate_tracked): facts whose provenance tokens are
-- internal gates over shared events -- materialised tracked views, joins
-- -- are handled natively, the capability no other exact ProvSQL method
-- offers with a width guarantee.  The tracked entry point walks the
-- circuit slice from the real provenance tokens; results are
-- cross-checked against ProvSQL's own ladder (probability_evaluate) on
-- the same instance (the thesis §7.3 correlated oracle).

CREATE TABLE base(k int);
SELECT add_provenance('base');
INSERT INTO base VALUES (0),(1),(2);
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM base; END $$;

-- R(0) = e0 AND e1 (an internal gate), S(0,2) = e0 (SHARED with R),
-- T(2) = e2.  The H0 query R(x),S(x,y),T(y) collapses to e0&e1&e2, so
-- its probability is 0.5^3 = 0.125 -- not the 0.5^3 an independent
-- reading would also give here, but the join through the shared e0 is
-- the point: an independent method would need the joint structure.
CREATE TABLE r AS SELECT 0 AS x FROM base a JOIN base b ON true
  WHERE a.k=0 AND b.k=1;
CREATE TABLE s AS SELECT 0 AS x, 2 AS y FROM base WHERE k=0;
CREATE TABLE t AS SELECT 2 AS y FROM base WHERE k=2;

SELECT round(ucq_joint_evaluate_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[
     {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,1,2], ARRAY[0, 0,2, 2], ARRAY[1,2,1],
  ARRAY[(SELECT provsql FROM r),(SELECT provsql FROM s),
        (SELECT provsql FROM t)])::numeric, 6) AS h0_correlated;

-- Ladder oracle: the real provenance of the H0 join, evaluated by
-- probability_evaluate.  Must agree.
CREATE TABLE h0 AS SELECT r.x, t.y FROM r, s, t
  WHERE r.x=s.x AND s.y=t.y;
SELECT round((SELECT probability_evaluate(provsql) FROM h0)::numeric, 6)
  AS ladder_oracle;

-- Architecturally-primary route: the compiler MATERIALISES the certified
-- d-D as ordinary provenance gates, and the answer comes through the
-- standard probability_evaluate on the returned token -- one evaluation
-- pipeline for the whole system.  Must equal the direct compilation.
SELECT round(probability_evaluate(ucq_joint_materialize_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[
     {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,1,2], ARRAY[0, 0,2, 2], ARRAY[1,2,1],
  ARRAY[(SELECT provsql FROM r),(SELECT provsql FROM s),
        (SELECT provsql FROM t)]))::numeric, 6) AS materialize_then_probeval;

-- Correlation matters: two R rows BOTH gated by e0.  "exists x R(x)" is
-- Pr[e0] = 0.5, NOT the 1-(1-0.5)^2 = 0.75 an independent reading gives.
CREATE TABLE r2 AS
  SELECT 10 AS x FROM base WHERE k=0
  UNION ALL SELECT 20 FROM base WHERE k=0;
SELECT round(ucq_joint_evaluate_tracked(
  '{"disjuncts":[{"n_vars":1,"atoms":[{"rel":0,"vars":[0]}]}]}'::jsonb,
  ARRAY[0,0], ARRAY[10,20], ARRAY[1,1],
  ARRAY[(SELECT provsql FROM r2 WHERE x=10),
        (SELECT provsql FROM r2 WHERE x=20)])::numeric, 6)
  AS shared_event_exists;

-- Width columns: the joint screen sees the correlation (data and circuit
-- widths are small here, but the fact-to-gate edges are what the joint
-- graph captures -- thesis Prop. 4.2.11).
SELECT joint_treewidth, data_treewidth_lb, circuit_treewidth_lb
FROM ucq_joint_compile_stats_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[
     {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,1,2], ARRAY[0, 0,2, 2], ARRAY[1,2,1],
  ARRAY[(SELECT provsql FROM r),(SELECT provsql FROM s),
        (SELECT provsql FROM t)]);

-- BID (repair_key) correlation: a provenance token can be a gate_mulinput
-- -- a mutually-exclusive categorical value.  The slice walk stick-breaks
-- each block into shared independent coins (mirroring
-- BooleanCircuit::rewriteMultivaluedGates), so the values of one block are
-- correctly exclusive in the joint encoding.  Three bounding boxes:
-- bbox 1 holds only deer (0.8), bbox 3 only fox (0.6), bbox 2 is a block
-- with two competing values deer (0.2) and fox (0.7) -- so a deer-via-bbox2
-- and a fox-via-bbox2 can never co-occur.
CREATE TABLE detection_n2 (photo_id int, bbox_key text, species_id int,
                           confidence float8);
INSERT INTO detection_n2 VALUES
  (1,'1',1,0.8),(1,'2',3,0.7),(1,'2',1,0.2),(1,'3',3,0.6);
SELECT repair_key('detection_n2','bbox_key');
DO $$ BEGIN PERFORM set_prob(provenance(), confidence) FROM detection_n2; END $$;

-- H0 = exists a species-1 detection AND a species-3 detection of photo 1.
-- rel0 = species-1 rows {bbox 1, bbox 2}; rel1 = species-3 rows {bbox 2,
-- bbox 3}; bbox keys are the element ids (1,2,3).  The exclusivity of
-- bbox 2's two values makes the answer
-- P((deer1 OR deer2) AND (fox2 OR fox3)) = 0.728, NOT the independent
-- 0.8.. reading.
SELECT round(ucq_joint_evaluate_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0,1,1], ARRAY[1,2,2,3], ARRAY[1,1,1,1],
  ARRAY[(SELECT provsql FROM detection_n2 WHERE bbox_key='1' AND species_id=1),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='2' AND species_id=1),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='2' AND species_id=3),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='3' AND species_id=3)]
  )::numeric, 6) AS bid_h0;

-- Ladder oracle on the native BID circuit (joint_width off): the
-- hand-computed 0.728 -- the joint-width answer must agree.
SET provsql.joint_width = off;
CREATE TABLE bid_h0 AS SELECT provenance() tok
  FROM (SELECT DISTINCT 1 FROM detection_n2 d1 JOIN detection_n2 d2 USING (photo_id)
        WHERE d1.species_id=1 AND d2.species_id=3) q;
SELECT round((SELECT probability_evaluate(tok) FROM bid_h0)::numeric, 6)
  AS bid_ladder_oracle;
SET provsql.joint_width = on;

-- ----------------------------------------------------------------------
-- Per-answer SINGLE SWEEP over correlated inputs
-- (ucq_joint_answers_swept_tracked).  The H0 query q(x) :- R(x),S(x,y),T(y)
-- has one answer per x; the tokens are walked once, the correlated joint
-- encoding + tree decomposition are built once, and each answer is a
-- head-pinned merged DP sweep.  Two answers, one of them correlated:
--   x=0 : R(0)=e0&e1, S(0,2)=e0 (e0 SHARED with R(0)), T(2)=e2
--         -> P = P(e0&e1&e2) = 0.5^3 = 0.125
--   x=5 : R(5)=e3, S(5,7)=e3, T(7)=e3 (all THREE share e3)
--         -> P = P(e3) = 0.5
-- The shared events make neither answer an independent product; the answer
-- set is cross-checked against the standard ladder evaluated per group.
CREATE TABLE base2(k int);
SELECT add_provenance('base2');
INSERT INTO base2 VALUES (0),(1),(2),(3);
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM base2; END $$;

CREATE TABLE r3 AS
  SELECT 0 AS x FROM base2 a JOIN base2 b ON true WHERE a.k=0 AND b.k=1
  UNION ALL SELECT 5 AS x FROM base2 WHERE k=3;
CREATE TABLE s3 AS
  SELECT 0 AS x, 2 AS y FROM base2 WHERE k=0
  UNION ALL SELECT 5 AS x, 7 AS y FROM base2 WHERE k=3;
CREATE TABLE t3 AS
  SELECT 2 AS y FROM base2 WHERE k=2
  UNION ALL SELECT 7 AS y FROM base2 WHERE k=3;

-- Ladder oracle: per-group probability of the H0 join, standard evaluation.
-- Strip the provenance column the CTAS adds so the cross-check below joins
-- two plain relations (a tracked relation joined with a multi-output SRF is
-- not rewritable by the ProvSQL hook).
SET provsql.joint_width = off;
CREATE TABLE h0_groups AS
  SELECT r3.x AS x, probability_evaluate(provenance()) AS p
    FROM r3, s3, t3 WHERE r3.x = s3.x AND s3.y = t3.y GROUP BY r3.x;
SELECT remove_provenance('h0_groups');
SET provsql.joint_width = on;

\echo '== correlated single-sweep per-answer probabilities =='
CREATE TABLE swept_corr AS
  SELECT head[1] AS x, probability AS p
  FROM ucq_joint_answers_swept_tracked(
    '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
    ARRAY[0], ARRAY[0, 5],
    ARRAY[0,0,1,1,2,2], ARRAY[0, 5, 0,2, 5,7, 2, 7], ARRAY[1,1,2,2,1,1],
    ARRAY[(SELECT provsql FROM r3 WHERE x=0),(SELECT provsql FROM r3 WHERE x=5),
          (SELECT provsql FROM s3 WHERE x=0),(SELECT provsql FROM s3 WHERE x=5),
          (SELECT provsql FROM t3 WHERE y=2),(SELECT provsql FROM t3 WHERE y=7)]);
SELECT x, round(p::numeric, 6) AS p FROM swept_corr ORDER BY x;

\echo '== correlated single-sweep == ladder per group: n_answers, max |diff| =='
SELECT count(*) AS n_answers, max(abs(sw.p - og.p)) AS max_abs_diff
FROM swept_corr sw JOIN h0_groups og USING (x);

-- Full top-down single DP over correlated inputs
-- (ucq_joint_answers_onedp_tracked): ONE bottom-up sweep over the joint
-- data+circuit decomposition emits one root per answer; an answer is held
-- open until its whole connected component (elements and gate vertices) has
-- left the bag, so every witness gate has folded into its provenance.
-- Discovers the answers; must match the per-binding sweep and the ladder.
\echo '== correlated top-down single DP per-answer probabilities =='
CREATE TABLE onedp_corr AS
  SELECT head[1] AS x, probability AS p
  FROM ucq_joint_answers_onedp_tracked(
    '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
    ARRAY[0],
    ARRAY[0,0,1,1,2,2], ARRAY[0, 5, 0,2, 5,7, 2, 7], ARRAY[1,1,2,2,1,1],
    ARRAY[(SELECT provsql FROM r3 WHERE x=0),(SELECT provsql FROM r3 WHERE x=5),
          (SELECT provsql FROM s3 WHERE x=0),(SELECT provsql FROM s3 WHERE x=5),
          (SELECT provsql FROM t3 WHERE y=2),(SELECT provsql FROM t3 WHERE y=7)]);
SELECT x, round(p::numeric, 6) AS p FROM onedp_corr ORDER BY x;

\echo '== correlated single DP == ladder per group: n_answers, max |diff| =='
SELECT count(*) AS n_answers, max(abs(od.p - og.p)) AS max_abs_diff
FROM onedp_corr od JOIN h0_groups og USING (x);
