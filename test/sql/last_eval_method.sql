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

SELECT remove_provenance('lem');
DROP TABLE lem;
