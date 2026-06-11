\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql,public;
SET provsql.provenance = 'boolean';

-- The correlated regime of the joint-width UCQ compiler: facts whose
-- provenance tokens are internal gates over shared events -- materialised
-- tracked views, joins -- are handled natively, the capability no other
-- exact ProvSQL method offers with a width guarantee.  The compiler walks
-- the circuit slice from the real provenance tokens; the width columns come
-- from ucq_joint_compile_stats_tracked and the materialised d-D from
-- ucq_joint_materialize_tracked, each cross-checked against ProvSQL's own
-- ladder (probability_evaluate) on the same instance (the thesis §7.3
-- correlated oracle).

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

SELECT round((SELECT probability FROM ucq_joint_compile_stats_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[
     {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,1,2], ARRAY[0, 0,2, 2], ARRAY[1,2,1],
  ARRAY[(SELECT provsql FROM r),(SELECT provsql FROM s),
        (SELECT provsql FROM t)]))::numeric, 6) AS h0_correlated;

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
SELECT round((SELECT probability FROM ucq_joint_compile_stats_tracked(
  '{"disjuncts":[{"n_vars":1,"atoms":[{"rel":0,"vars":[0]}]}]}'::jsonb,
  ARRAY[0,0], ARRAY[10,20], ARRAY[1,1],
  ARRAY[(SELECT provsql FROM r2 WHERE x=10),
        (SELECT provsql FROM r2 WHERE x=20)]))::numeric, 6)
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
SELECT round((SELECT probability FROM ucq_joint_compile_stats_tracked(
  '{"disjuncts":[{"n_vars":2,"atoms":[{"rel":0,"vars":[0]},{"rel":1,"vars":[1]}]}]}'::jsonb,
  ARRAY[0,0,1,1], ARRAY[1,2,2,3], ARRAY[1,1,1,1],
  ARRAY[(SELECT provsql FROM detection_n2 WHERE bbox_key='1' AND species_id=1),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='2' AND species_id=1),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='2' AND species_id=3),
        (SELECT provsql FROM detection_n2 WHERE bbox_key='3' AND species_id=3)]
  ))::numeric, 6) AS bid_h0;

-- Ladder oracle on the native BID circuit (joint_width off): the
-- hand-computed 0.728 -- the joint-width answer must agree.
SET provsql.joint_width = off;
CREATE TABLE bid_h0 AS SELECT provenance() tok
  FROM (SELECT DISTINCT 1 FROM detection_n2 d1 JOIN detection_n2 d2 USING (photo_id)
        WHERE d1.species_id=1 AND d2.species_id=3) q;
SELECT round((SELECT probability_evaluate(tok) FROM bid_h0)::numeric, 6)
  AS bid_ladder_oracle;
SET provsql.joint_width = on;

-- The same BID H0 through the TRANSPARENT route (provenance() under
-- joint_width on), so the stick-broken exclusion blocks travel the
-- *materialisation* path, not just probabilityEvaluation: each synthetic
-- stick-breaking coin must be materialised as a fresh independent input
-- gate carrying its probability.  Regression guard: the materialiser used
-- to choke on the synthetic leaf token and silently fall back to the
-- literal circuit (so the value still matched but joint-width never fired);
-- here we assert the token *differs* from the joint_width-off circuit and
-- that 'independent' accepts the certified d-D and returns the 0.728 oracle.
CREATE TABLE bid_h0_on AS SELECT provenance() tok
  FROM (SELECT DISTINCT 1 FROM detection_n2 d1 JOIN detection_n2 d2 USING (photo_id)
        WHERE d1.species_id=1 AND d2.species_id=3) q;
SELECT remove_provenance('bid_h0_on');
\echo '== BID H0 transparent: joint-width fired (token differs from off) =='
SELECT ((SELECT tok FROM bid_h0_on) <> (SELECT tok FROM bid_h0)) AS joint_width_fired;
\echo '== BID H0 transparent: independent on the materialised d-D = oracle =='
SELECT round(probability_evaluate((SELECT tok FROM bid_h0_on), 'independent')::numeric, 6)
  AS jw_independent;

-- ----------------------------------------------------------------------
-- Per-answer over correlated inputs.  The H0 query q(x) :- R(x),S(x,y),T(y)
-- has one answer per x; the transparent route gathers the tokens once and
-- materialises every answer's d-D in a single merged DP sweep.  Two answers,
-- both correlated:
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

-- Per-answer over CORRELATED inputs through the TRANSPARENT route: the flat
-- GROUP BY query under provsql.joint_width = on substitutes the head-pinned
-- joint-width provenance (one gather + one single-DP sweep materialising all
-- answers); off is the exact ladder.  Two answers, both correlated:
--   x=0 : R(0)=e0&e1, S(0,2)=e0 (e0 SHARED with R(0)), T(2)=e2 -> 0.5^3 = 0.125
--   x=5 : R(5)=e3, S(5,7)=e3, T(7)=e3 (all THREE share e3) -> P(e3) = 0.5
-- The shared events make neither answer an independent product.
SET provsql.joint_width = on;
CREATE TABLE jc_on AS
  SELECT r3.x AS x, probability_evaluate(provenance()) AS p
    FROM r3, s3, t3 WHERE r3.x = s3.x AND s3.y = t3.y GROUP BY r3.x;
SELECT remove_provenance('jc_on');
SET provsql.joint_width = off;
CREATE TABLE jc_off AS
  SELECT r3.x AS x, probability_evaluate(provenance()) AS p
    FROM r3, s3, t3 WHERE r3.x = s3.x AND s3.y = t3.y GROUP BY r3.x;
SELECT remove_provenance('jc_off');
SET provsql.joint_width = on;

\echo '== correlated transparent per-answer probabilities =='
SELECT x, round(p::numeric, 6) AS p FROM jc_on ORDER BY x;

\echo '== correlated transparent == ladder per group: n_answers, max |diff| =='
SELECT count(*) AS n_answers, max(abs(a.p - b.p)) AS max_abs_diff
FROM jc_on a JOIN jc_off b USING (x);
