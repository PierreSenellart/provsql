\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Provenance for recursive queries (WITH RECURSIVE), PG15+.
--
-- The planner hook lowers a recursive CTE whose body touches a provenance-
-- tracked relation into a fixpoint evaluation (provsql.eval_recursive): the
-- user writes a plain recursive query and the result carries provenance like
-- any provenance-tracked SELECT.  Each round re-evaluates `base UNION
-- recursive` through ProvSQL's own rewriting, so the recursive join yields
-- `times` gates, the untracked base branch yields gate_one, and the UNION
-- yields the `plus` merge of alternative derivations.

-- A probabilistic DAG of edges + named edge variables.
CREATE TABLE redge(src int, dst int, p float8, label text);
INSERT INTO redge(src,dst,p,label) VALUES
  (1,2,0.9,'e12'), (1,3,0.5,'e13'), (2,3,0.8,'e23'), (2,4,0.6,'e24'), (3,4,0.7,'e34');
SELECT add_provenance('redge');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM redge; END $$;
SELECT create_provenance_mapping('redge_labels', 'redge', 'label');

-- Transitive reachability from node 1: Boolean provenance expression and the
-- (exact, possible-worlds) reachability probability per reachable node.  The
-- result is materialised and stripped of its provsql column (random input-gate
-- UUIDs) so the comparison is on the deterministic formula/probability.
CREATE TABLE reach_result AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM redge e JOIN reach r ON e.src = r.node
  )
  SELECT node,
         sr_formula(provenance(), 'redge_labels') AS boolean_expression,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6) AS probability
  FROM reach;
SELECT remove_provenance('reach_result');
SELECT * FROM reach_result ORDER BY node;
DROP TABLE reach_result;

-- s-t reachability (source 1, target 4): a filter on the recursive result.
CREATE TABLE st_result AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM redge e JOIN reach r ON e.src = r.node
  )
  SELECT node, sr_formula(provenance(), 'redge_labels') AS boolean_expression
  FROM reach WHERE node = 4;
SELECT remove_provenance('st_result');
SELECT * FROM st_result;
DROP TABLE st_result;

DROP TABLE redge;

-- Cyclic data, default (no boolean_provenance): the circuit never stabilises
-- structurally, so the round guard fires.  Shown via a direct driver call with
-- a small round bound.
CREATE TABLE cedge(src int, dst int);
INSERT INTO cedge VALUES (1,2), (2,1);
SELECT add_provenance('cedge');
\set VERBOSITY terse
SELECT provsql.eval_recursive(
  'SELECT 1 UNION SELECT e.dst FROM cedge e JOIN cyc r ON e.src = r.node',
  'cyc', 'node', 'node integer', 3);
\set VERBOSITY default
DROP TABLE cedge;

-- Cyclic data under provsql.boolean_provenance: the provenance value converges
-- (absorptive), so reachability is computed.  Graph has a 2->3->2 cycle; the
-- back-edge c32 appears only in cyclic (non-minimal) derivations and is
-- therefore absorbed -- the reachability probabilities do not depend on it.
-- Expected (hand-computed, independent of c32): reach(2)=0.9, reach(3)=0.72,
-- reach(4)=0.9*(1-(1-0.6)*(1-0.8*0.7))=0.7416.
SET provsql.boolean_provenance = on;
CREATE TABLE cyc_edge(src int, dst int, p float8);
INSERT INTO cyc_edge VALUES (1,2,0.9), (2,3,0.8), (3,2,0.5), (2,4,0.6), (3,4,0.7);
SELECT add_provenance('cyc_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM cyc_edge; END $$;
CREATE TABLE cyc_result AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM cyc_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node,
         round(probability_evaluate(provenance())::numeric, 6)                AS reliability,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6) AS exact
  FROM reach;
SELECT remove_provenance('cyc_result');
SELECT * FROM cyc_result ORDER BY node;
DROP TABLE cyc_result;
DROP TABLE cyc_edge;
SET provsql.boolean_provenance = off;

-- A recursive term with a set-returning function in its target list
-- (e.g. SELECT unnest(...)) is rejected with the usual unsupported-shape
-- error rather than crashing the backend.  The driver would otherwise build
-- a per-round INSERT ... SELECT ... UNION SELECT srf(...) whose planning
-- leaves a NULL expr in PostgreSQL's PathTarget (a SIGSEGV in
-- get_expr_width); lower_recursive_cte bails out on the SRF instead.
CREATE TABLE srf_edge(src int, dst int);
INSERT INTO srf_edge VALUES (1, 2), (2, 3);
SELECT add_provenance('srf_edge');
\set VERBOSITY terse
WITH RECURSIVE reach(node) AS (
    SELECT 1
  UNION
    SELECT unnest(ARRAY[e.dst]) FROM srf_edge e JOIN reach r ON e.src = r.node
)
SELECT node FROM reach ORDER BY node;
\set VERBOSITY default
DROP TABLE srf_edge;
