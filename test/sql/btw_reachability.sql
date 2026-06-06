\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Exact two-terminal network reliability on bounded-treewidth data.
--
-- reachability_probability compiles s-t reachability along a tree
-- decomposition of the *data* graph (the provenance refinement of
-- Courcelle's theorem), emitting a d-DNNF that is deterministic and
-- decomposable by construction, of size linear in the number of edges
-- for fixed data treewidth.  This sidesteps the relational-plan lineage
-- entirely: it is exact and linear-time on cyclic graphs (where the
-- WITH RECURSIVE fixpoint cannot terminate structurally) and on graph
-- families whose lineage circuit treewidth grows with the data.

-- A probabilistic diamond DAG (same data as the recursive test).
CREATE TABLE btw_edge(src int, dst int, p float8);
INSERT INTO btw_edge VALUES
  (1,2,0.9), (1,3,0.5), (2,3,0.8), (2,4,0.6), (3,4,0.7);
SELECT add_provenance('btw_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btw_edge; END $$;

-- Directed s-t reliability; the same value as the recursive-CTE route
-- (recursive.sql evaluates node 4 to 0.801800).
SELECT round(reachability_probability('btw_edge','src','dst',1,4)::numeric, 6)
  AS diamond_directed;

-- Cross-check inside this test: the lineage of "a path from 1 to 4
-- exists", built by hand as a UNION of the three paths, evaluated by
-- possible worlds.
CREATE TABLE btw_check AS
  SELECT round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6)
         AS possible_worlds
  FROM (
      SELECT 1 AS ok FROM btw_edge a, btw_edge b
      WHERE a.src=1 AND a.dst=2 AND b.src=2 AND b.dst=4
    UNION
      SELECT 1 FROM btw_edge a, btw_edge b
      WHERE a.src=1 AND a.dst=3 AND b.src=3 AND b.dst=4
    UNION
      SELECT 1 FROM btw_edge a, btw_edge b, btw_edge c
      WHERE a.src=1 AND a.dst=2 AND b.src=2 AND b.dst=3 AND c.src=3 AND c.dst=4
  ) paths;
SELECT remove_provenance('btw_check');
SELECT * FROM btw_check;
DROP TABLE btw_check;

-- Undirected reading of the same edges, reverse direction, trivial cases.
SELECT round(reachability_probability('btw_edge','src','dst',1,4,false)::numeric, 6)
  AS diamond_undirected;
SELECT round(reachability_probability('btw_edge','src','dst',4,1)::numeric, 6)
  AS diamond_reverse;
SELECT round(reachability_probability('btw_edge','src','dst',1,1)::numeric, 6)
  AS source_equals_target;
SELECT round(reachability_probability('btw_edge','src','dst',1,7)::numeric, 6)
  AS isolated_target;

-- Compilation statistics: the diamond has treewidth 2 and 5 edge
-- variables; the structural counters are bounded but heuristic- and
-- platform-dependent, so only sanity bounds are printed.
SELECT data_treewidth,
       nb_variables,
       nb_bags BETWEEN 1 AND 6 AS bags_bounded,
       max_states BETWEEN 1 AND 100 AS states_bounded,
       nb_gates BETWEEN 5 AND 200 AS gates_bounded,
       round(probability::numeric, 6) AS probability
FROM reachability_compile_stats('btw_edge','src','dst',1,4);
DROP TABLE btw_edge;

-- Cyclic graph (the boolean_provenance example of the recursive test:
-- expected 0.7416, independent of the cycle's back-edge c32).  No
-- boolean_provenance machinery is needed here: the decomposition-aligned
-- compilation handles cyclic data natively.
CREATE TABLE btw_cyc(src int, dst int, p float8);
INSERT INTO btw_cyc VALUES
  (1,2,0.9), (2,3,0.8), (3,2,0.5), (2,4,0.6), (3,4,0.7);
SELECT add_provenance('btw_cyc');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btw_cyc; END $$;
SELECT round(reachability_probability('btw_cyc','src','dst',1,4)::numeric, 6)
  AS cyclic_directed;
DROP TABLE btw_cyc;

-- Series chain of 20 edges: reliability 0.9^20.
CREATE TABLE btw_chain(src int, dst int);
INSERT INTO btw_chain SELECT i, i+1 FROM generate_series(1,20) i;
SELECT add_provenance('btw_chain');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.9) FROM btw_chain; END $$;
SELECT round(reachability_probability('btw_chain','src','dst',1,21)::numeric, 6)
  AS series_20,
  round(0.9^20, 6) AS expected;
-- Without set_prob, tuples default to probability 1.
CREATE TABLE btw_sure(src int, dst int);
INSERT INTO btw_sure SELECT i, i+1 FROM generate_series(1,5) i;
SELECT add_provenance('btw_sure');
SELECT round(reachability_probability('btw_sure','src','dst',1,6)::numeric, 6)
  AS certain_chain;
DROP TABLE btw_sure;
DROP TABLE btw_chain;

-- Three parallel edges between the terminals: 1 - 0.5^3.
CREATE TABLE btw_par(src int, dst int);
INSERT INTO btw_par VALUES (1,2), (1,2), (1,2);
SELECT add_provenance('btw_par');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btw_par; END $$;
SELECT round(reachability_probability('btw_par','src','dst',1,2)::numeric, 6)
  AS parallel_3;
DROP TABLE btw_par;

-- Text-valued vertices, undirected triangle:
-- P(a ~ c) = P(ac) + (1-P(ac)) P(ab) P(bc) = 0.625.
CREATE TABLE btw_tri(f text, t text);
INSERT INTO btw_tri VALUES ('a','b'), ('b','c'), ('a','c');
SELECT add_provenance('btw_tri');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btw_tri; END $$;
SELECT round(reachability_probability('btw_tri','f','t','a'::text,'c'::text,false)::numeric, 6)
  AS triangle_undirected;
DROP TABLE btw_tri;

-- Columnar form: an undirected edge can also be encoded as a
-- mutual-reverse pair of arcs sharing their provenance token.
SELECT round(reachability_evaluate(
  ARRAY[1,2, 2,3, 1,3],
  ARRAY[2,1, 3,2, 3,1],
  ARRAY['11111111-1111-1111-1111-111111111111','11111111-1111-1111-1111-111111111111',
        '22222222-2222-2222-2222-222222222222','22222222-2222-2222-2222-222222222222',
        '33333333-3333-3333-3333-333333333333','33333333-3333-3333-3333-333333333333']::uuid[],
  ARRAY[0.5,0.5, 0.5,0.5, 0.5,0.5],
  1, 3, true)::numeric, 6) AS triangle_reverse_pairs;

-- A 2 x 30 ladder (treewidth 2), undirected: 59 verticals of
-- probability 0.9 and 30 rungs of probability 0.5, corner to opposite
-- corner.  The d-DNNF stays linear in the edge count (the lineage-based
-- route would exceed the circuit-treewidth cap here).
CREATE TABLE btw_ladder(src int, dst int, p float8);
INSERT INTO btw_ladder
  SELECT 2*i+1, 2*i+3, 0.9 FROM generate_series(0,28) i
  UNION ALL
  SELECT 2*i+2, 2*i+4, 0.9 FROM generate_series(0,28) i
  UNION ALL
  SELECT 2*i+1, 2*i+2, 0.5 FROM generate_series(0,29) i;
SELECT add_provenance('btw_ladder');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btw_ladder; END $$;
SELECT data_treewidth,
       nb_variables,
       nb_gates < 100 * nb_variables AS gates_linear,
       max_states <= 32 AS states_bounded,
       round(probability::numeric, 6) AS probability
FROM reachability_compile_stats('btw_ladder','src','dst',1,60,false);
DROP TABLE btw_ladder;

-- Error paths.
\set VERBOSITY terse
-- Derived provenance (a view): edges must be independent base tuples.
CREATE TABLE btw_base(src int, dst int);
INSERT INTO btw_base VALUES (1,2),(2,3);
SELECT add_provenance('btw_base');
CREATE VIEW btw_view AS
  SELECT e1.src, e2.dst FROM btw_base e1 JOIN btw_base e2 ON e1.dst = e2.src;
SELECT reachability_probability('btw_view','src','dst',1,3);
DROP VIEW btw_view;
DROP TABLE btw_base;
-- Data treewidth above the supported limit (K14 has treewidth 13).
CREATE TABLE btw_clique(src int, dst int);
INSERT INTO btw_clique
  SELECT i, j FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j;
SELECT add_provenance('btw_clique');
SELECT reachability_probability('btw_clique','src','dst',1,14,false);
DROP TABLE btw_clique;
-- A token shared by edges that are not mutual reverses.
SELECT reachability_evaluate(
  ARRAY[1,2], ARRAY[2,3],
  ARRAY['11111111-1111-1111-1111-111111111111',
        '11111111-1111-1111-1111-111111111111']::uuid[],
  ARRAY[0.5,0.5], 1, 3, true);
-- NULL terminals.
CREATE TABLE btw_one(src int, dst int);
INSERT INTO btw_one VALUES (1,2);
SELECT add_provenance('btw_one');
SELECT reachability_probability('btw_one','src','dst',NULL::int,2);
DROP TABLE btw_one;
\set VERBOSITY default
