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
-- Its SQLSTATE and tag, read from the raised error rather than from the
-- message, which VERBOSITY terse above leaves out (its CONTEXT carries the
-- deparsed round, whose text differs between PostgreSQL versions).  A recursion
-- whose rounds do not end has no provenance to give, so the refusal is
-- deliberate; the bag recursion answers the same way.
DO $$
DECLARE d text;
BEGIN
  PERFORM provsql.eval_recursive(
    'SELECT 1 UNION SELECT e.dst FROM cedge e JOIN cyc r ON e.src = r.node',
    'cyc', 'node', 'node integer', 3);
EXCEPTION WHEN feature_not_supported THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
  RAISE NOTICE '% / %', SQLSTATE, d;
END $$;
DO $$
DECLARE d text;
BEGIN
  PERFORM provsql.eval_recursive_all(
    'SELECT 1', 'SELECT e.dst FROM cedge e JOIN cyc2 r ON e.src = r.node',
    'cyc2', 'cyc2_all', 'node', 'node integer', 3);
EXCEPTION WHEN feature_not_supported THEN
  GET STACKED DIAGNOSTICS d = PG_EXCEPTION_DETAIL;
  RAISE NOTICE '% / %', SQLSTATE, d;
END $$;
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

-- The tables a recursion works in are named for us, not after the CTE, and they
-- live no longer than the statement: the temporary schema is searched BEFORE
-- the search path, so a leftover named after the CTE answered a later statement
-- of the same session -- with the rows of a statement that had ended, in place
-- of the user's own relation of that name, and without a word.
CREATE TABLE leak_e(id int, parent int);
INSERT INTO leak_e VALUES (1, NULL), (2, 1), (3, 2);
SELECT add_provenance('leak_e');
CREATE TABLE leak_r AS
  WITH RECURSIVE leak_t(id) AS (
      SELECT id FROM leak_e WHERE parent IS NULL
    UNION ALL
      SELECT e.id FROM leak_e e JOIN leak_t ON e.parent = leak_t.id)
  SELECT count(*)::text AS n FROM leak_t;
SELECT remove_provenance('leak_r');
SELECT n FROM leak_r;
SELECT count(*) AS temporary_relations_left FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
 WHERE n.nspname LIKE 'pg_temp%';
SELECT count(*) FROM leak_t;
DROP TABLE leak_r, leak_e;

-- A body whose columns include a provsql one -- SELECT * over a tracked
-- relation, expanded before any hook of ours can hide it -- would give the
-- working table two columns of that name, and the token the star asks for is
-- not data the rounds carry: refused, where the same recursion written with
-- explicit columns answers.  The refusal names the STAR as the cause and is
-- scoped a gap, not the shape of the recursion: the recursion is fine (the
-- reference may sit on either side of the join, and the semantics translates
-- such a query), so what the user can act on is writing the columns out.
CREATE TABLE bag_star(id int, parent_id int);
INSERT INTO bag_star VALUES (1, NULL), (2, 1), (3, 2);
SELECT add_provenance('bag_star');
WITH RECURSIVE t(id, parent_id) AS (
    SELECT * FROM bag_star WHERE id = 1
  UNION ALL
    SELECT b.* FROM bag_star b JOIN t ON t.id = b.parent_id)
SELECT count(*) FROM t;
CREATE TABLE bag_r AS
  WITH RECURSIVE t(id) AS (
      SELECT id FROM bag_star WHERE id = 1
    UNION ALL
      SELECT b.id FROM bag_star b JOIN t ON t.id = b.parent_id)
  SELECT count(*)::text AS n FROM t;
SELECT remove_provenance('bag_r');
SELECT n FROM bag_r;
DROP TABLE bag_r, bag_star;

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

-- A bound read from a TRACKED relation: the comparison against one value of a
-- tracked relation is itself tracked (the value is the one-row aggregation over
-- that relation, cross-joined), so outside a recursion the row needs both rows
-- and weighs 0.5 * 0.5.  Inside one, an uncertain bound would leave no round
-- empty and the rounds would not end, so the bound is read as plain SQL with
-- the warning that says so, and the answer is the one of the query with that
-- relation untracked: the three rows of the actual bound, each carrying the
-- seed's probability.
SELECT add_provenance('bag_param');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM bag_param; END $$;
CREATE TABLE bag_r AS SELECT v FROM bag_seed WHERE v < (SELECT n FROM bag_param);
SET provsql.active = off;
SELECT v, round(probability(provsql)::numeric, 6) AS p FROM bag_r ORDER BY v;
SET provsql.active = on;
DROP TABLE bag_r;
CREATE TABLE bag_r AS
  WITH RECURSIVE c(n) AS (
      SELECT v FROM bag_seed
    UNION ALL
      SELECT n + 1 FROM c WHERE n < (SELECT n FROM bag_param))
  SELECT n FROM c;
SET provsql.active = off;
SELECT n, round(probability(provsql)::numeric, 6) AS p FROM bag_r ORDER BY n;
SET provsql.active = on;
DROP TABLE bag_r;
-- The freezing of the bound is COHERENT: the relation the bound reads is read
-- nowhere else in the statement, so what the answer gives is the provenance of
-- the query with that relation untracked, and provsql.implicit_freeze = 'error'
-- leaves it a warning rather than refusing the query.  The bound is marked on
-- the copy of the body that is deparsed for the rounds, so the node reported is
-- not one of the statement's own; the coherence test recognises it by its shape
-- there, and only there (a statement that holds two identical subqueries, one
-- frozen and one tracked, still reports the relation as read both ways).
SET provsql.implicit_freeze = 'error';
CREATE TABLE bag_r AS
  WITH RECURSIVE c(n) AS (
      SELECT v FROM bag_seed
    UNION ALL
      SELECT n + 1 FROM c WHERE n < (SELECT n FROM bag_param))
  SELECT n FROM c;
RESET provsql.implicit_freeze;
SELECT remove_provenance('bag_r');
SELECT count(*) AS rows_under_error FROM bag_r;
DROP TABLE bag_r;
DROP TABLE bag_e, bag_seed, bag_param;

-- A SIBLING CTE read from inside the RECURSIVE term.  The lowering deparses the
-- two terms into standalone SQL for eval_recursive_all to run round by round, so
-- every name in them has to resolve on its own.  A sibling read as a range-table
-- entry was inlined -- inline_ctes_in_rtable follows range tables -- and one read
-- from a SUBLINK was not: the walk that inlines a sublink's CTE references
-- ignores the CTE subqueries, so a sublink inside a CTE body was reached by
-- neither.  The recursive term therefore still named `starting`, and the INSERT
-- of each round raised a bare "relation \"starting\" does not exist", with no
-- DETAIL and no tag: difftest probe/recursive-sibling-cte, dba/175868.
-- The inlining of a CTE body's sublinks now runs BEFORE the range-table pass,
-- which is the pass that lowers a recursive CTE and freezes its text.
-- Three rows at one half in a chain, 1 <- 2 <- 3: the first is reached by the
-- non-recursive term and carries its own half, the second needs both it and its
-- parent (0.25), the third the whole chain (0.125).  The bound itself is read on
-- the data as it is and says so -- an uncertain bound would leave no round
-- empty, so the rounds would not end.
CREATE TABLE rsib(id int, name text, parent_id int);
INSERT INTO rsib VALUES (1,'Father',NULL), (2,'Son',1), (3,'Grandson',2);
SELECT add_provenance('rsib');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM rsib; END $$;
CREATE TABLE rsib_r AS
  WITH RECURSIVE starting(id) AS (SELECT id FROM rsib WHERE name = 'Father'),
       d(id) AS (SELECT id FROM starting
                 UNION ALL
                 SELECT t.id FROM rsib t JOIN d ON t.parent_id = d.id
                   WHERE t.id > (SELECT min(id) FROM starting))
  SELECT id, probability(provenance()) AS p FROM d;
SELECT remove_provenance('rsib_r');
SELECT id, round(p::numeric, 6) AS p FROM rsib_r ORDER BY id;
DROP TABLE rsib_r;
-- The same rows plain SQL gives, which is what the row set has to be.
SET provsql.active = off;
WITH RECURSIVE starting(id) AS (SELECT id FROM rsib WHERE name = 'Father'),
     d(id) AS (SELECT id FROM starting
               UNION ALL
               SELECT t.id FROM rsib t JOIN d ON t.parent_id = d.id
                 WHERE t.id > (SELECT min(id) FROM starting))
SELECT id FROM d ORDER BY id;
SET provsql.active = on;
-- The control that was never broken: the sibling read in the NON-recursive term
-- only, where it is a range-table entry and was inlined all along.  No bound, so
-- no reading of one: the same three rows and the same chain.
CREATE TABLE rsib_r AS
  WITH RECURSIVE starting(id) AS (SELECT id FROM rsib WHERE name = 'Father'),
       d(id) AS (SELECT id FROM starting
                 UNION ALL
                 SELECT t.id FROM rsib t JOIN d ON t.parent_id = d.id)
  SELECT id, probability(provenance()) AS p FROM d;
SELECT remove_provenance('rsib_r');
SELECT id, round(p::numeric, 6) AS p FROM rsib_r ORDER BY id;
DROP TABLE rsib_r;
SELECT remove_provenance('rsib');
DROP TABLE rsib;
