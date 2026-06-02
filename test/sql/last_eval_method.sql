\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

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
SET provsql.boolean_provenance = off;
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
RESET provsql.boolean_provenance;

SELECT remove_provenance('lem');
DROP TABLE lem;
