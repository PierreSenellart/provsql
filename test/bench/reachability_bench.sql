-- ----------------------------------------------------------------------
-- test/bench/reachability_bench.sql
--
-- Route benchmark for the bounded-treewidth reachability compiler: which
-- method the cost chooser resolves to, per requested guarantee, on an
-- ordinary WITH RECURSIVE reachability query over treelike data.
--
-- The data is a directed ladder: two rails 1..n and n+1..2n, rung edges
-- between them, so the Gaifman graph of the edge relation has treewidth 2
-- for every n -- bounded, but not a tree (the ladder is 2-connected, and
-- the reverse rungs make it cyclic; the compiler is native on cycles, no
-- fixpoint involved, whereas the generic recursive evaluation reaches
-- them only under an absorptive-or-lower provenance class).  Under
-- provsql.provenance = 'boolean' the recursive-CTE lowering recognises
-- the linear reachability shape and compiles, along a tree decomposition
-- of the *data* graph, one certified d-D per reachable vertex; the
-- chooser then reads the persisted certificate and the linear
-- 'independent' pass serves every guarantee ("exact when cheap": the
-- exact value trivially meets any tolerance).
--
-- Deliberately small data -- the point is the route taken and its
-- stability across guarantees, not scale; ucq_joint_bench.sql is the
-- scaling counterpart on the data side.
--
-- Run:
--   createdb reachbench && psql reachbench -X -f test/bench/reachability_bench.sql
--
-- Reference picks (this machine, PostgreSQL 18, provsql 1.12.0-dev;
-- n = 24, edge probability 0.9, target vertex 48):
--
--   route         exact        rel eps=.1   rel eps=.3   additive     det d=0      route_off
--   reachability  independent  independent  independent  independent  independent  tree-decomposition
--
-- One method for every guarantee, ~32 ms, P = 0.748201.  No GUC disables
-- the route -- it fires for every absorptive-or-lower class, 'boolean'
-- and 'absorptive' alike -- so the route_off leg raises the class to
-- 'semiring' instead, where the generic eval_recursive fixpoint builds
-- the literal circuit and the chooser has to pay for it
-- (tree-decomposition, P = 0.288396).  That leg necessarily runs the
-- ACYCLIC twin of the ladder, since cyclic data has no finite semiring
-- provenance: the comparison isolates the compiler, not the cycle.
-- ----------------------------------------------------------------------
\set ECHO none
\timing off
SET search_path TO provsql_test, provsql, public;

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
CREATE SCHEMA IF NOT EXISTS provsql_test;

DROP TABLE IF EXISTS reach_edge CASCADE;
CREATE TABLE reach_edge(src int, dst int);

-- Ladder on 2n vertices: rails 1..n and n+1..2n (forward), rungs both
-- ways (so the reachability relation is genuinely cyclic).
DO $$
DECLARE n int := 24;
BEGIN
  INSERT INTO reach_edge SELECT i, i+1 FROM generate_series(1, n-1) i;
  INSERT INTO reach_edge SELECT n+i, n+i+1 FROM generate_series(1, n-1) i;
  INSERT INTO reach_edge SELECT i, n+i FROM generate_series(1, n) i;
  INSERT INTO reach_edge SELECT n+i, i FROM generate_series(1, n) i;
END $$;
SELECT add_provenance('reach_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.9) FROM reach_edge; END $$;

-- The same ladder with one-way rungs only: still treewidth 2, but now
-- acyclic, so the generic fixpoint terminates and the route_off leg
-- lands on a method rather than on an error.
DROP TABLE IF EXISTS reach_dag CASCADE;
CREATE TABLE reach_dag(src int, dst int);
DO $$
DECLARE n int := 24;
BEGIN
  INSERT INTO reach_dag SELECT i, i+1 FROM generate_series(1, n-1) i;
  INSERT INTO reach_dag SELECT n+i, n+i+1 FROM generate_series(1, n-1) i;
  INSERT INTO reach_dag SELECT i, n+i FROM generate_series(1, n) i;
END $$;
SELECT add_provenance('reach_dag');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.9) FROM reach_dag; END $$;

DROP TABLE IF EXISTS reach_res CASCADE;
CREATE TABLE reach_res(request text, method text, prob numeric, ms double precision);

DO $$
DECLARE
  v double precision; i int; t0 timestamptz;
  -- the four guarantees of the paper's route table, plus the
  -- no-failure (delta=0) request, as in dtree_bench.sql.
  reqs text[][] := ARRAY[['exact',NULL,'exact'],
                         ['relative','epsilon=0.1,delta=0.05','rel_tight'],
                         ['relative','epsilon=0.3,delta=0.2','rel_loose'],
                         ['additive','epsilon=0.1,delta=0.05','additive'],
                         ['additive','epsilon=0.1,delta=0','det_d0']];
BEGIN
  -- ===== route ON: 'boolean' licenses the decomposition-aligned driver ====
  PERFORM set_config('provsql.provenance','boolean',false);
  CREATE TEMP TABLE _reach1 AS
    WITH RECURSIVE reach(node) AS (
        SELECT 1
      UNION
        SELECT e.dst FROM reach_edge e JOIN reach r ON e.src = r.node
    )
    SELECT node, provenance() AS p FROM reach;
  PERFORM remove_provenance('_reach1');

  FOR i IN 1..array_length(reqs,1) LOOP
    PERFORM set_config('provsql.last_eval_method','',false);
    t0 := clock_timestamp();
    SELECT round(probability_evaluate(p, reqs[i][1], reqs[i][2])::numeric,6)
      INTO v FROM _reach1 WHERE node = 48;
    INSERT INTO reach_res VALUES (reqs[i][3],
      current_setting('provsql.last_eval_method'), v,
      extract(epoch FROM clock_timestamp()-t0)*1000);
  END LOOP;

  -- ===== route OFF: no GUC disables the route (it fires for every
  -- absorptive-or-lower class, 'boolean' and 'absorptive' alike), so the
  -- comparison runs the class ABOVE it, 'semiring', where the generic
  -- eval_recursive fixpoint builds the literal circuit and the chooser
  -- faces that instead.  On the acyclic ladder: cyclic data has no finite
  -- semiring provenance, so it is the class, not the cycle, that decides
  -- what this leg can measure. =========================================
  BEGIN
    PERFORM set_config('provsql.provenance','semiring',false);
    CREATE TEMP TABLE _reach0 AS
      WITH RECURSIVE reach(node) AS (
          SELECT 1
        UNION
          SELECT e.dst FROM reach_dag e JOIN reach r ON e.src = r.node
      )
      SELECT node, provenance() AS p FROM reach;
    PERFORM remove_provenance('_reach0');
    PERFORM set_config('provsql.last_eval_method','',false);
    t0 := clock_timestamp();
    SELECT round(probability_evaluate(p)::numeric,6) INTO v FROM _reach0 WHERE node = 48;
    INSERT INTO reach_res VALUES ('route_off',
      current_setting('provsql.last_eval_method'), v,
      extract(epoch FROM clock_timestamp()-t0)*1000);
  EXCEPTION WHEN OTHERS THEN
    INSERT INTO reach_res VALUES ('route_off', 'ERROR: '||SQLERRM, NULL, NULL);
  END;
END $$;
RESET provsql.provenance;

\pset format aligned
\echo '--- resolved method per guarantee (reachability route) ---'
\echo '--- (route_off runs the acyclic twin: no finite semiring provenance on cycles) ---'
SELECT request, method, prob, round(ms::numeric,1) AS ms
FROM reach_res
ORDER BY CASE request WHEN 'exact' THEN 1 WHEN 'rel_tight' THEN 2
                      WHEN 'rel_loose' THEN 3 WHEN 'additive' THEN 4
                      WHEN 'det_d0' THEN 5 ELSE 6 END;

\echo '--- one line, uniform with the paper route table ---'
SELECT 'reachability' AS route,
       max(method) FILTER (WHERE request='exact')     AS exact,
       max(method) FILTER (WHERE request='rel_tight') AS rel_tight,
       max(method) FILTER (WHERE request='rel_loose') AS rel_loose,
       max(method) FILTER (WHERE request='additive')  AS additive,
       max(method) FILTER (WHERE request='det_d0')    AS det_d0,
       max(method) FILTER (WHERE request='route_off') AS route_off
FROM reach_res;

DROP TABLE reach_res;
SELECT remove_provenance('reach_edge'); SELECT remove_provenance('reach_dag');
DROP TABLE reach_edge, reach_dag;
