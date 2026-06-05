\set ECHO none
-- Three user-facing probability paths: a granted TOLERANCE, not a named
-- algorithm.  'exact' is an alias for the empty/default method; 'relative'
-- promises (1±eps); 'additive' promises |p̂-p| <= eps.  Admissibility nests
-- exact ⊂ relative ⊂ additive, so a relative/additive request returns an EXACT
-- value when one is cheaply available ("exact when cheaper": a tuple-independent
-- circuit resolves via 'independent', reported as such).  Only when no cheap
-- exact applies does the path fall to its estimator (stopping-rule / monte-carlo).
-- See src/probability_evaluate.cpp (the three-path dispatch).
\pset format unaligned
SET search_path TO provsql_test,provsql;
SET provsql.boolean_provenance = off;
SET provsql.monte_carlo_seed = 42;

CREATE TABLE pp(id int);
INSERT INTO pp VALUES (1),(2),(3),(4);
SELECT add_provenance('pp');
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.5) FROM pp WHERE id=1;
  PERFORM set_prob(provsql, 0.3) FROM pp WHERE id=2;
  PERFORM set_prob(provsql, 0.4) FROM pp WHERE id=3;
  PERFORM set_prob(provsql, 0.1) FROM pp WHERE id=4;
END $$;
DO $$
DECLARE x1 uuid; x2 uuid; x3 uuid; x4 uuid;
BEGIN
  SELECT provsql INTO x1 FROM pp WHERE id=1;
  SELECT provsql INTO x2 FROM pp WHERE id=2;
  SELECT provsql INTO x3 FROM pp WHERE id=3;
  SELECT provsql INTO x4 FROM pp WHERE id=4;
  -- independent (disjoint clauses): (x1&x2)|(x3&x4) = 0.184 -- 'independent'
  -- resolves it exactly, so every path returns the exact value.
  PERFORM set_config('pp.indep',  provenance_plus(ARRAY[provenance_times(x1,x2), provenance_times(x3,x4)])::text, false);
  -- NOT independent (x1 shared): (x1&x2)|(x1&x3) = 0.29 -- the relative/additive
  -- paths fall to their estimator.
  PERFORM set_config('pp.shared', provenance_plus(ARRAY[provenance_times(x1,x2), provenance_times(x1,x3)])::text, false);
END $$;
\set indep  '(current_setting(''pp.indep'')::uuid)'
\set shared '(current_setting(''pp.shared'')::uuid)'

-- 'exact' is an alias for the empty/default method (bit-identical: same chooser).
-- It agrees with the explicit 'independent' too, but only up to floating-point
-- round-off -- the calibrated chooser may pick a different exact method
-- (e.g. possible-worlds, summing over worlds rather than multiplying), so the
-- value matches to round-off, not bit-for-bit.
SELECT probability_evaluate(:indep, 'exact') = probability_evaluate(:indep)             AS exact_alias_default,
       abs(probability_evaluate(:indep, 'exact') - probability_evaluate(:indep,'independent')) < 1e-12 AS exact_alias_independent;

-- Exact-when-cheaper, now generalised to the whole exact portfolio (not just
-- 'independent'): on a circuit a cheap EXACT method resolves, the relative and
-- additive paths return the EXACT value (within FP tolerance of 'independent'),
-- reported as the cost-ranked exact method -- no sampling.  Here :indep is a tiny
-- 2-clause DNF, cheapest under 'sieve'; the value differs from 'independent' only
-- at round-off (inclusion-exclusion vs multiplication), so this is a tolerance test.
SET provsql.last_eval_method='';
SELECT abs(probability_evaluate(:indep,'relative','epsilon=0.1') - probability_evaluate(:indep,'independent')) < 1e-12 AS relative_is_exact;
SHOW provsql.last_eval_method;
SET provsql.last_eval_method='';
SELECT abs(probability_evaluate(:indep,'additive','epsilon=0.1') - probability_evaluate(:indep,'independent')) < 1e-12 AS additive_is_exact;
SHOW provsql.last_eval_method;

-- :shared is a non-read-once 2-clause DNF over 3 inputs: 'independent' is
-- inapplicable, but the cheapest admissible method is still EXACT possible-worlds
-- (2^3 worlds, cheaper than sampling at this size), comfortably within the granted
-- tolerance of the exact value (0.29).
SET provsql.last_eval_method='';
SELECT abs(probability_evaluate(:shared,'relative','epsilon=0.05,delta=0.01') - 0.29) <= 0.05 AS relative_in_tol;
SHOW provsql.last_eval_method;
SET provsql.last_eval_method='';
SELECT abs(probability_evaluate(:shared,'additive','epsilon=0.02,delta=0.05') - 0.29) <= 0.05 AS additive_in_tol;
SHOW provsql.last_eval_method;

-- A monotone CNF (AND of overlapping 4-var OR windows) over 24 inputs:
-- 'independent' throws (shared vars), 'sieve' does not apply (CNF, not DNF), and
-- 2^24 possible-worlds is dear.  The generalised d-tree (dtreeBoundsCircuit)
-- recurses on this CNF directly, and its cheap-feature cost estimate (S/eps)
-- rates it cheapest -- but it actually does ~4e5 subproblems here.  Speculative
-- execution catches that: the chooser budgets the d-tree at the next-best
-- method's cost (tree-decomposition, which is genuinely fast on this
-- low-treewidth band), the d-tree exceeds the budget and bails, and the chooser
-- escalates to tree-decomposition -- the correct, faster choice.  So the
-- approximate paths report tree-decomposition, and the estimate stays in [0,1].
CREATE TABLE ppcnf(id int);
INSERT INTO ppcnf SELECT generate_series(1,24);
SELECT add_provenance('ppcnf');
DO $$ BEGIN PERFORM set_prob(provsql, 0.5) FROM ppcnf; END $$;
SET provsql.active = off;
DO $$
DECLARE v uuid[]; cl uuid; acc uuid; i int;
BEGIN
  SELECT array_agg(provsql::uuid ORDER BY id) INTO v FROM ppcnf;
  acc := NULL;
  FOR i IN 1..21 LOOP
    cl := provenance_plus(ARRAY[v[i],v[i+1],v[i+2],v[i+3]]);
    IF acc IS NULL THEN acc := cl; ELSE acc := provenance_times(acc, cl); END IF;
  END LOOP;
  PERFORM set_config('pp.cnf', acc::text, false);
END $$;
RESET provsql.active;
\set cnf '(current_setting(''pp.cnf'')::uuid)'
SET provsql.last_eval_method='';
SELECT probability_evaluate(:cnf,'relative','epsilon=0.2,delta=0.1') BETWEEN 0 AND 1 AS relative_estimates;
SHOW provsql.last_eval_method;
SET provsql.last_eval_method='';
SELECT probability_evaluate(:cnf,'additive','epsilon=0.2,delta=0.1') BETWEEN 0 AND 1 AS additive_estimates;
SHOW provsql.last_eval_method;
DROP TABLE ppcnf;

-- RV guard lets the paths through (a relative/additive request on a circuit the
-- exact methods cannot touch is allowed); a bad name is still refused.
SELECT probability_evaluate(:shared,'nonsense-method');

DROP TABLE pp;
RESET provsql.boolean_provenance;
RESET provsql.monte_carlo_seed;
