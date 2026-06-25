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

SELECT remove_provenance('lem');
DROP TABLE lem;
