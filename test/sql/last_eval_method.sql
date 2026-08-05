\set ECHO none
\pset format unaligned

-- provsql.last_eval_method records, after each probability_evaluate call, the
-- evaluation method that produced the result (comma-separated and deduplicated
-- across the session).  We only print the method name, never the probability,
-- so the expected output stays deterministic (monte-carlo in particular gives
-- a non-deterministic value but a fixed method label).
CREATE TABLE lem(id int);
INSERT INTO lem SELECT generate_series(1,16);
SELECT add_provenance('lem');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM lem; END $$;

-- Explicit method: independent.
SET provsql.last_eval_method = '';
DO $$ BEGIN PERFORM probability_evaluate(provenance(), 'independent') FROM lem; END $$;
SHOW provsql.last_eval_method;

-- Explicit method: monte-carlo (value discarded; only the label is checked).
SET provsql.last_eval_method = '';
DO $$ BEGIN PERFORM probability_evaluate(provenance(), 'monte-carlo', '100') FROM lem; END $$;
SHOW provsql.last_eval_method;

-- Explicit method: possible-worlds.
SET provsql.last_eval_method = '';
DO $$ BEGIN PERFORM probability_evaluate(provenance(), 'possible-worlds') FROM lem; END $$;
SHOW provsql.last_eval_method;

-- Repeated calls with the same method are deduplicated.
SET provsql.last_eval_method = '';
DO $$ BEGIN
  PERFORM probability_evaluate(provenance(), 'independent') FROM lem;
  PERFORM probability_evaluate(provenance(), 'independent') FROM lem;
END $$;
SHOW provsql.last_eval_method;

-- Distinct methods accumulate, in call order.
SET provsql.last_eval_method = '';
DO $$ BEGIN
  PERFORM probability_evaluate(provenance(), 'independent') FROM lem;
  PERFORM probability_evaluate(provenance(), 'possible-worlds') FROM lem;
END $$;
SHOW provsql.last_eval_method;

-- Default (cost-driven) chooser on two larger circuits (sized above the small-N
-- crossover where the cheap-constant possible-worlds otherwise wins).  Reports
-- the route actually taken -- exercising the makeDD decomposition, the feature
-- framework (DnfShape / TreewidthProxy) and the calibrated estimatedCost.
-- boolean_provenance off so the load-time folding leaves the shapes intact.
SET provsql.provenance = 'semiring';
SET provsql.active = off;
DO $$
DECLARE v uuid[]; acc uuid; ors uuid[];
BEGIN
  SELECT array_agg(provsql::uuid ORDER BY id) INTO v FROM lem;
  -- (a) read-once but NON-DNF (alternating AND/OR over 10 distinct inputs):
  --     'independent' resolves it in linear time, the cheapest at this size.
  acc := v[1];
  FOR i IN 2..10 LOOP
    IF i % 2 = 0 THEN acc := provenance_times(acc, v[i]);
    ELSE acc := provenance_plus(ARRAY[acc, v[i]]); END IF;
  END LOOP;
  PERFORM set_config('lem.indep', acc::text, false);
  -- (b) non-read-once, non-DNF ladder AND_i (v_i OR v_{i+1}) over 15 inputs
  --     (treewidth 2): 'independent' throws, sieve does not apply, and after
  --     acquiring the degeneracy proxy the chooser finds tree-decomposition
  --     cheaper than enumerating 2^15 worlds.
  ors := ARRAY[]::uuid[];
  FOR i IN 1..14 LOOP ors := ors || provenance_plus(ARRAY[v[i], v[i+1]]); END LOOP;
  acc := ors[1];
  FOR i IN 2..14 LOOP acc := provenance_times(acc, ors[i]); END LOOP;
  PERFORM set_config('lem.ladder', acc::text, false);
END $$;
RESET provsql.active;

SET provsql.last_eval_method = '';
SELECT probability_evaluate(current_setting('lem.indep')::uuid) IS NOT NULL AS ran;
SHOW provsql.last_eval_method;

SET provsql.last_eval_method = '';
SELECT probability_evaluate(current_setting('lem.ladder')::uuid) IS NOT NULL AS ran;
SHOW provsql.last_eval_method;
RESET provsql.provenance;

-- Möbius route: q9/QW is a safe UCQ tractable only because the #P-hard term of
-- its inclusion-exclusion expansion cancels; a gate_mobius root is evaluated by
-- the dedicated 'mobius' method.  That route returns early, before the main
-- recording path, so this guards that last_eval_method is still set -- both for
-- the default chooser and the explicit method (regression for the early-return
-- that left it empty).
SET provsql.provenance = 'boolean';
SET provsql.mobius = on;
SET provsql.joint_width = off;
CREATE TABLE q9r(x int);  INSERT INTO q9r SELECT i FROM generate_series(1,3) i;
CREATE TABLE q9t(y int);  INSERT INTO q9t SELECT j FROM generate_series(1,3) j;
CREATE TABLE q9s1(x int, y int); CREATE TABLE q9s2(x int, y int); CREATE TABLE q9s3(x int, y int);
INSERT INTO q9s1 SELECT i,j FROM generate_series(1,3) i, generate_series(1,3) j;
INSERT INTO q9s2 SELECT i,j FROM generate_series(1,3) i, generate_series(1,3) j;
INSERT INTO q9s3 SELECT i,j FROM generate_series(1,3) i, generate_series(1,3) j;
SELECT add_provenance('q9r'); SELECT add_provenance('q9t');
SELECT add_provenance('q9s1'); SELECT add_provenance('q9s2'); SELECT add_provenance('q9s3');
DO $$ BEGIN PERFORM set_prob(provenance(),0.1) FROM q9r; PERFORM set_prob(provenance(),0.1) FROM q9t;
  PERFORM set_prob(provenance(),0.1) FROM q9s1; PERFORM set_prob(provenance(),0.1) FROM q9s2;
  PERFORM set_prob(provenance(),0.1) FROM q9s3; END $$;
CREATE TEMP TABLE q9tok AS SELECT provenance() AS p FROM (
    SELECT 1 FROM q9r, q9s1 a1, q9s3 a3, q9t t3 WHERE q9r.x=a1.x AND a3.y=t3.y
    UNION SELECT 1 FROM q9s1 b1, q9s2 b2, q9s3 b3, q9t tb WHERE b1.x=b2.x AND b1.y=b2.y AND b3.y=tb.y
    UNION SELECT 1 FROM q9s2 c2, q9s3 c3, q9s3 c3b, q9t tc WHERE c2.x=c3.x AND c2.y=c3.y AND c3b.y=tc.y
    UNION SELECT 1 FROM q9r d, q9s1 d1, q9s1 d1b, q9s2 d2, q9s2 d2b, q9s3 d3
      WHERE d.x=d1.x AND d1b.x=d2.x AND d1b.y=d2.y AND d2b.x=d3.x AND d2b.y=d3.y) qq;
SELECT remove_provenance('q9tok');

-- default (cost-driven) chooser routes the gate_mobius root to 'mobius'
SET provsql.last_eval_method = '';
SELECT probability_evaluate(p) IS NOT NULL AS ran FROM q9tok;
SHOW provsql.last_eval_method;

-- explicit 'mobius' likewise records
SET provsql.last_eval_method = '';
SELECT probability_evaluate(p, 'mobius') IS NOT NULL AS ran FROM q9tok;
SHOW provsql.last_eval_method;

DROP TABLE q9tok;
SELECT remove_provenance('q9r'); SELECT remove_provenance('q9t');
SELECT remove_provenance('q9s1'); SELECT remove_provenance('q9s2'); SELECT remove_provenance('q9s3');
DROP TABLE q9r, q9t, q9s1, q9s2, q9s3;
RESET provsql.provenance; RESET provsql.mobius; RESET provsql.joint_width;

-- The three planner-time routes -- the safe-query (read-once) rewriter, the
-- joint-width UCQ compiler and the reachability compiler -- each replace a
-- query's ordinary lineage with a circuit of their own and then hand it to
-- this same dispatcher, which evaluates all three by the independent /
-- certified-island sweep.  Each stamps a route tag on the root it produces
-- (provsql_route), so they report under their own names instead of all
-- collapsing into 'independent'.

-- (a) sq-rewrite: a hierarchical CQ q(x) :- A(x), B(x) with multiple matches
--     per side.  Only the rewrite makes the per-id circuit read-once; the tag
--     is what separates it from a circuit that was read-once to begin with.
--     Twelve duplicates per side per id put the per-group circuit above the
--     small-N crossover where the cheap-constant possible-worlds would
--     otherwise underbid every linear method.
SET provsql.provenance = 'boolean';
SET provsql.joint_width = off;
CREATE TABLE lem_l(id int); CREATE TABLE lem_r(id int);
INSERT INTO lem_l SELECT 1 FROM generate_series(1,12);
INSERT INTO lem_l SELECT 2 FROM generate_series(1,12);
INSERT INTO lem_r SELECT 1 FROM generate_series(1,12);
INSERT INTO lem_r SELECT 2 FROM generate_series(1,12);
SELECT add_provenance('lem_l'); SELECT add_provenance('lem_r');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM lem_l;
            PERFORM set_prob(provsql, 0.4) FROM lem_r; END $$;
CREATE TEMP TABLE lem_sq AS
  SELECT provenance() AS p FROM lem_l l, lem_r r WHERE l.id = r.id GROUP BY l.id;
SELECT remove_provenance('lem_sq');

SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_sq WHERE probability_evaluate(p) IS NOT NULL;
SHOW provsql.last_eval_method;

-- explicit 'sq-rewrite' likewise records
SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_sq WHERE probability_evaluate(p, 'sq-rewrite') IS NOT NULL;
SHOW provsql.last_eval_method;

-- 'independent' stays available by name on a tagged root: the escape hatch
-- names the computation rather than its producer (applicable() is only
-- consulted by the auto-chooser).
SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_sq WHERE probability_evaluate(p, 'independent') IS NOT NULL;
SHOW provsql.last_eval_method;

DROP TABLE lem_sq;
SELECT remove_provenance('lem_l'); SELECT remove_provenance('lem_r');
DROP TABLE lem_l, lem_r;

-- (b) bounded-jw: H0 = R(x), S(x,y), T(y) is the canonical unsafe (#P-hard)
--     query the Dalvi-Suciu dichotomy rules out from lifted inference; its
--     existence provenance is replaced by the joint-width compiler's certified
--     d-D, whose root carries the route tag in info2 next to DNNF_CERT_INFO.
SET provsql.mobius = off;
SET provsql.joint_width = on;
CREATE TABLE lem_h0r(x int); CREATE TABLE lem_h0s(x int, y int); CREATE TABLE lem_h0t(y int);
INSERT INTO lem_h0r VALUES (1),(2);
INSERT INTO lem_h0s VALUES (1,1),(1,2),(2,2);
INSERT INTO lem_h0t VALUES (1),(2);
SELECT add_provenance('lem_h0r'); SELECT add_provenance('lem_h0s'); SELECT add_provenance('lem_h0t');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM lem_h0r;
            PERFORM set_prob(provsql, 0.5) FROM lem_h0s;
            PERFORM set_prob(provsql, 0.5) FROM lem_h0t; END $$;
CREATE TEMP TABLE lem_jw AS
  SELECT provenance() AS p FROM (
    SELECT DISTINCT 1 AS one FROM lem_h0r, lem_h0s, lem_h0t
     WHERE lem_h0r.x = lem_h0s.x AND lem_h0s.y = lem_h0t.y) q;
SELECT remove_provenance('lem_jw');

SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_jw WHERE probability_evaluate(p) IS NOT NULL;
SHOW provsql.last_eval_method;

-- explicit 'bounded-jw' likewise records
SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_jw WHERE probability_evaluate(p, 'bounded-jw') IS NOT NULL;
SHOW provsql.last_eval_method;

DROP TABLE lem_jw;
SELECT remove_provenance('lem_h0r'); SELECT remove_provenance('lem_h0s');
SELECT remove_provenance('lem_h0t');
DROP TABLE lem_h0r, lem_h0s, lem_h0t;
RESET provsql.provenance; RESET provsql.mobius; RESET provsql.joint_width;

-- (c) reachability: the columnar entry point, so this stays version-independent
--     (the user-facing WITH RECURSIVE form is covered by btw_recursive).  Its
--     root is the 'absorptive' assumption wrapper the compiler mints, tagged in
--     info1 -- the assumption kind alone would not identify the route, since
--     the truncated-fixpoint path mints 'absorptive' wrappers too.
CREATE TEMP TABLE lem_reach AS
  SELECT token AS p FROM reachability_materialize(
    ARRAY[1,1,2,2,3], ARRAY[2,3,3,4,4],
    (SELECT array_agg(public.uuid_generate_v5(uuid_ns_provsql(), 'lemreach'||i))
       FROM generate_series(1,5) i),
    ARRAY[0.9,0.5,0.8,0.6,0.7], NULL, NULL,
    ARRAY[1], ARRAY['00000000-0000-0000-0000-000000000000']::uuid[],
    ARRAY[1.0], true);

SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_reach WHERE probability_evaluate(p) IS NOT NULL;
SHOW provsql.last_eval_method;

-- explicit 'reachability' likewise records
SET provsql.last_eval_method = '';
SELECT count(*) AS ran FROM lem_reach WHERE probability_evaluate(p, 'reachability') IS NOT NULL;
SHOW provsql.last_eval_method;

-- A route method named on a root that route did not produce is an error, not a
-- silent evaluation under the wrong label.
SET provsql.last_eval_method = '';
DO $$ BEGIN PERFORM probability_evaluate(provenance(), 'bounded-jw') FROM lem; END $$;
SHOW provsql.last_eval_method;

DROP TABLE lem_reach;

SELECT remove_provenance('lem');
DROP TABLE lem;
