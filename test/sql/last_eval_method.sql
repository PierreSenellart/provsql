\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- provsql.last_eval_method records, after each probability_evaluate call, the
-- evaluation method that produced the result (comma-separated and deduplicated
-- across the session).  We only print the method name, never the probability,
-- so the expected output stays deterministic (monte-carlo in particular gives
-- a non-deterministic value but a fixed method label).
CREATE TABLE lem(id int);
INSERT INTO lem VALUES (1),(2),(3);
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

-- Default (empty) method resolves to independent on a tuple-independent circuit.
SET provsql.last_eval_method = '';
DO $$ BEGIN PERFORM probability_evaluate(provenance()) FROM lem; END $$;
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

-- Default method on a small NON-independent circuit: 'independent' throws, and
-- the cost-ordered exact chooser picks 'possible-worlds' (cheap 2^N for small N,
-- ahead of tree-decomposition / compilation).  last_eval_method reports the
-- route actually taken -- exercising both the makeDD decomposition and the
-- small-N cost heuristic.  boolean_provenance off so the load-time folding
-- leaves the shared-variable shape intact.
SET provsql.boolean_provenance = off;
DO $$
DECLARE x1 uuid; x2 uuid; x3 uuid;
BEGIN
  SELECT provsql INTO x1 FROM lem WHERE id=1;
  SELECT provsql INTO x2 FROM lem WHERE id=2;
  SELECT provsql INTO x3 FROM lem WHERE id=3;
  -- (x1 AND x2) OR (x1 AND x3): x1 is shared, so the circuit is not read-once;
  -- 'independent' throws and tree-decomposition resolves it.
  PERFORM set_config('lem.shared', provenance_plus(ARRAY[
            provenance_times(x1,x2), provenance_times(x1,x3)])::text, false);
END $$;
SET provsql.last_eval_method = '';
SELECT probability_evaluate(current_setting('lem.shared')::uuid) IS NOT NULL AS ran;
SHOW provsql.last_eval_method;
RESET provsql.boolean_provenance;

SELECT remove_provenance('lem');
DROP TABLE lem;
