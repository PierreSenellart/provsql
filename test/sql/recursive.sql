\set ECHO none
\pset format unaligned

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
SET provsql.provenance = 'boolean';
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
SET provsql.provenance = 'semiring';

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

-- Regression (crash): with provsql.active = off the planner hook must stand
-- back and let a WITH RECURSIVE over a tracked relation plan as ordinary SQL.
-- It must not drive the fixpoint (eval_recursive): that runs SPI / temp-table
-- creation at plan time and its per-round INSERT ... SELECT formerly crashed
-- the backend under active = off (a synthesized provsql target entry left with
-- a NULL expr that the planner dereferenced).
CREATE TABLE aedge(src int, dst int);
INSERT INTO aedge VALUES (1,2),(2,3),(3,4);
SELECT add_provenance('aedge');
SET provsql.active = off;
WITH RECURSIVE r(node) AS (
    SELECT dst FROM aedge WHERE src = 1
  UNION
    SELECT e.dst FROM aedge e JOIN r ON e.src = r.node
)
SELECT count(*) FROM r;
SET provsql.active = on;
DROP TABLE aedge;

-- Regression (error): a recursive CTE referenced in two arms of a top-level
-- UNION, each requesting provenance(), must be lowered -- and its fixpoint
-- temp table created -- exactly once.  Re-lowering per arm DROPped and
-- recreated the temp table, leaving the first arm's already-analyzed scan
-- bound to a stale OID ("could not open relation with OID ...").
CREATE TABLE dedge(src int, dst int, label text);
INSERT INTO dedge(src,dst,label) VALUES (1,2,'a'), (2,3,'b'), (3,4,'c');
SELECT add_provenance('dedge');
SELECT create_provenance_mapping('dedge_labels', 'dedge', 'label');
CREATE TABLE twoarm_result AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM dedge e JOIN reach r ON e.src = r.node
  )
    SELECT node, sr_formula(provenance(),'dedge_labels') AS f FROM reach WHERE node = 4
  UNION
    SELECT node, sr_formula(provenance(),'dedge_labels') AS f FROM reach WHERE node = 2;
SELECT remove_provenance('twoarm_result');
SELECT * FROM twoarm_result ORDER BY node;
DROP TABLE twoarm_result;
DROP TABLE dedge;

-- A recursive CTE reads another CTE of the WITH, defined after it: it is
-- inlined into the body the fixpoint evaluates.
CREATE TABLE sib_e(src int, dst int);
INSERT INTO sib_e VALUES (1, 2), (2, 3), (3, 4);
SELECT add_provenance('sib_e');
CREATE TABLE sib_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT dst FROM edges WHERE src = 1
    UNION
      SELECT e.dst FROM edges e JOIN reach r ON e.src = r.node
  ), edges AS (SELECT src, dst FROM sib_e WHERE dst <> 4)
  SELECT node, present(provenance()) AS present FROM reach;
SELECT remove_provenance('sib_r');
SELECT * FROM sib_r ORDER BY node;
DROP TABLE sib_r;

-- A window function in the recursive term: not a monotone fixpoint, refused
-- up front.
WITH RECURSIVE r AS (
    SELECT src, 0 AS i FROM sib_e
  UNION
    SELECT e.src, CASE WHEN e.dst = lag(e.dst) OVER (ORDER BY e.src)
                       THEN r.i ELSE r.i + 1 END
    FROM sib_e e JOIN r ON e.src = r.src WHERE r.i < 3)
SELECT * FROM r;
DROP TABLE sib_e;

-- A UNION ALL recursion is the bag one: its rounds read the previous round,
-- its answer is the rows of every round together, and it ends on a round that
-- derives nothing.  Each row is one derivation and two derivations of a tuple
-- are two rows, where UNION returns one row annotated with their disjunction.
-- Over the edges 1->2, 1->3, 2->4, 3->4, each present with probability one
-- half, the seed {2, 3} reaches 4 by two paths: UNION ALL gives the row 4
-- twice, annotated 12 (x) 24 and 13 (x) 34, at 0.25 each, where UNION gives it
-- once at 1 - (1 - 0.25)^2 = 0.4375.
CREATE TABLE bag_e(src int, dst int);
INSERT INTO bag_e VALUES (1,2), (1,3), (2,4), (3,4);
SELECT add_provenance('bag_e');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM bag_e; END $$;
CREATE TABLE bag_r AS
  WITH RECURSIVE reach(n) AS (
      SELECT dst FROM bag_e WHERE src = 1
    UNION ALL
      SELECT e.dst FROM reach, bag_e e WHERE e.src = reach.n)
  SELECT n FROM reach;
SET provsql.active = off;
SELECT n, round(probability(provsql)::numeric, 6) AS p FROM bag_r ORDER BY n, p;
SET provsql.active = on;
DROP TABLE bag_r;
CREATE TABLE bag_r AS
  WITH RECURSIVE reach(n) AS (
      SELECT dst FROM bag_e WHERE src = 1
    UNION
      SELECT e.dst FROM reach, bag_e e WHERE e.src = reach.n)
  SELECT n FROM reach;
SET provsql.active = off;
SELECT n, round(probability(provsql)::numeric, 6) AS p FROM bag_r ORDER BY n, p;
SET provsql.active = on;
DROP TABLE bag_r;

-- The generators of the corpora: a counter, an array extended per round, a
-- string built up.  Every row derives from the one row of the seed, so each
-- carries its probability, and the bound ends the rounds.
CREATE TABLE bag_seed(v int);
INSERT INTO bag_seed VALUES (1);
SELECT add_provenance('bag_seed');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM bag_seed; END $$;
CREATE TABLE bag_param(n int);
INSERT INTO bag_param VALUES (3);
CREATE TABLE bag_r AS
  WITH RECURSIVE fill(n, arr) AS (
      SELECT v, ARRAY[v] FROM bag_seed
    UNION ALL
      SELECT n + 1, array_append(arr, n + 1) FROM fill
      WHERE n < (SELECT n * 2 FROM bag_param))
  SELECT n, arr FROM fill;
SET provsql.active = off;
SELECT n, arr::text AS arr, round(probability(provsql)::numeric, 6) AS p
FROM bag_r ORDER BY n;
SET provsql.active = on;
DROP TABLE bag_r;
CREATE TABLE bag_r AS
  WITH RECURSIVE s(n, t) AS (
      SELECT v, 'x'::text FROM bag_seed
    UNION ALL
      SELECT n + 1, t || 'x' FROM s WHERE n < 3)
  SELECT n, t FROM s;
SET provsql.active = off;
SELECT n, t, round(probability(provsql)::numeric, 6) AS p FROM bag_r ORDER BY n;
SET provsql.active = on;
DROP TABLE bag_r;

-- A bound read from a TRACKED relation is refused: each round is a query of
-- its own, and an uncorrelated scalar subquery compared in a WHERE clause is a
-- shape the decorrelation does not cover.  Asserted here without the recursion,
-- which raises the same refusal from inside the driver (its CONTEXT carries
-- the deparsed round, whose text is not the same in every PostgreSQL version).
SELECT add_provenance('bag_param');
SELECT v FROM bag_seed WHERE v < (SELECT n FROM bag_param);
DROP TABLE bag_e, bag_seed, bag_param;
