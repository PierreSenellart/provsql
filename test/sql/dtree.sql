\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The 'd-tree' method (src/DTree) refines the cheap leaf bound by independent-or
-- decomposition and Shannon expansion: a deterministic anytime engine that is
-- EXACT when run to a zero-width interval (the default, by name) and certified
-- additive when given an epsilon.  We assert invariants (equals the exact
-- baselines; within eps of exact) rather than brittle float values.
SET provsql.boolean_provenance = off;

CREATE TABLE dt(id int);
INSERT INTO dt SELECT generate_series(1,8);
SELECT add_provenance('dt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.1*id) FROM dt; END $$;

SET provsql.active = off;
DO $$
DECLARE x uuid[];
BEGIN
  SELECT array_agg(provsql::uuid ORDER BY id) INTO x FROM dt;
  -- Entangled (non-read-once) DNF: a 4-cycle (x1 x2)∨(x2 x3)∨(x3 x4)∨(x1 x4).
  -- Every variable is shared across clauses, so the leaf bound is loose and the
  -- engine must Shannon-expand to reach the exact value.
  PERFORM set_config('dt.ent', provenance_plus(ARRAY[
     provenance_times(x[1],x[2]), provenance_times(x[2],x[3]),
     provenance_times(x[3],x[4]), provenance_times(x[1],x[4])])::text, false);
  -- Read-once DNF (x1 x2)∨(x3 x4): disjoint, so the bound is already exact.
  PERFORM set_config('dt.ro', provenance_plus(ARRAY[
     provenance_times(x[1],x[2]), provenance_times(x[3],x[4])])::text, false);
  -- Non-DNF: AND-of-ORs (x1∨x2)(x3∨x4).
  PERFORM set_config('dt.cnf', provenance_times(
     provenance_plus(ARRAY[x[1],x[2]]),
     provenance_plus(ARRAY[x[3],x[4]]))::text, false);
  -- 8-cycle (treewidth 2, 8 clauses): the Shannon recursion reaches the same
  -- residual sub-paths along different branches, so this exercises the memoised
  -- shared-DAG path (Phase 2) and must still agree with the exact baseline.
  PERFORM set_config('dt.cyc', provenance_plus(ARRAY[
     provenance_times(x[1],x[2]), provenance_times(x[2],x[3]),
     provenance_times(x[3],x[4]), provenance_times(x[4],x[5]),
     provenance_times(x[5],x[6]), provenance_times(x[6],x[7]),
     provenance_times(x[7],x[8]), provenance_times(x[8],x[1])])::text, false);
END $$;
RESET provsql.active;

-- Exact d-tree matches the exact baselines (possible-worlds, sieve) on the
-- entangled circuit, and on the read-once one.
SELECT abs(probability_evaluate(current_setting('dt.ent')::uuid,'d-tree')
         - probability_evaluate(current_setting('dt.ent')::uuid,'possible-worlds'))
         < 1e-9 AS ent_eq_pw,
       abs(probability_evaluate(current_setting('dt.ent')::uuid,'d-tree')
         - probability_evaluate(current_setting('dt.ent')::uuid,'sieve'))
         < 1e-9 AS ent_eq_sieve,
       abs(probability_evaluate(current_setting('dt.ro')::uuid,'d-tree')
         - probability_evaluate(current_setting('dt.ro')::uuid,'possible-worlds'))
         < 1e-9 AS ro_eq_pw;

-- Memoised shared-DAG recursion (8-cycle) agrees with the exact baseline.
SELECT abs(probability_evaluate(current_setting('dt.cyc')::uuid,'d-tree')
         - probability_evaluate(current_setting('dt.cyc')::uuid,'tree-decomposition'))
         < 1e-9 AS cyc_eq_td;

-- Additive (by-name epsilon): the estimate is within eps of the exact value.
SELECT abs(probability_evaluate(current_setting('dt.ent')::uuid,'d-tree','epsilon=0.05')
         - probability_evaluate(current_setting('dt.ent')::uuid,'possible-worlds'))
         <= 0.05 AS add_within_eps;

-- Non-DNF circuit errors cleanly (the engine is DNF-restricted in Phase 1).
SELECT probability_evaluate(current_setting('dt.cnf')::uuid,'d-tree');

-- Deterministic (delta=0) admissibility: an 'additive' request with delta=0
-- excludes the (eps,delta) samplers, so the chooser settles on a DETERMINISTIC
-- method (an exact one when cheap, else the d-tree) and the value stays within
-- eps of exact.  We assert the route is not a sampler and the value is sound.
SET provsql.last_eval_method = '';
SELECT abs(probability_evaluate(current_setting('dt.ent')::uuid,'additive','epsilon=0.1,delta=0')
         - probability_evaluate(current_setting('dt.ent')::uuid,'possible-worlds'))
         <= 0.1 AS det_within_eps;
SELECT current_setting('provsql.last_eval_method')
         !~ '(monte-carlo|karp-luby|stopping-rule)' AS det_not_sampler;

-- A by-name sampler cannot honour delta=0 (infinite sample count): clean error.
SELECT probability_evaluate(current_setting('dt.ent')::uuid,'karp-luby','epsilon=0.1,delta=0');

SELECT remove_provenance('dt');
DROP TABLE dt;
RESET provsql.boolean_provenance;
