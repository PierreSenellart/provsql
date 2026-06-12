\set ECHO none
\pset format unaligned

-- Bounded-hop reachability: a hop-counting recursive CTE -- a counter
-- column seeded by an integer constant, incremented in the recursive
-- arm, and bounded by a mandatory WHERE qual -- compiles along the data
-- decomposition with walk-length-set states.  Row (v, h) carries the
-- provenance of "some walk of exactly h edges reaches v" (walks, not
-- paths: cycles pump lengths), matching the generic fixpoint's rows;
-- per vertex, the gate a hop-discarding query's deduplication addresses
-- is pre-created over the compilation's native within-bound root, so
-- the natural "within k hops" probability stays on the linear certified
-- route.

SET provsql.provenance = 'boolean';

-- Chain 1-2-3-4-5 (p=.5), bound 2: exact-length chain values, nothing
-- beyond two hops.
CREATE TABLE btwh_edge(src int, dst int);
INSERT INTO btwh_edge SELECT i, i+1 FROM generate_series(1,4) i;
SELECT add_provenance('btwh_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwh_edge; END $$;
CREATE TABLE btwh_r AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_edge e JOIN reach r ON e.src = r.node
      WHERE r.hops < 2
  )
  SELECT node, hops, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwh_r');
SELECT * FROM btwh_r ORDER BY node, hops;
DROP TABLE btwh_r;

-- Directed triangle 1->2->3->1 with tail 3->4 (p=.5), bound 4: the
-- cycle pumps walk lengths -- (1,3) is the walk back around the
-- triangle -- and the generic fixpoint (GUC off) agrees on every row.
CREATE TABLE btwh_tri(src int, dst int);
INSERT INTO btwh_tri VALUES (1,2),(2,3),(3,1),(3,4);
SELECT add_provenance('btwh_tri');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwh_tri; END $$;
CREATE TABLE btwh_tri_r AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_tri e JOIN reach r ON e.src = r.node
      WHERE r.hops < 4
  )
  SELECT node, hops,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6)
           AS prob
  FROM reach;
SELECT remove_provenance('btwh_tri_r');
SELECT * FROM btwh_tri_r ORDER BY node, hops;
SET provsql.provenance = 'semiring';
CREATE TABLE btwh_tri_r2 AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_tri e JOIN reach r ON e.src = r.node
      WHERE r.hops < 4
  )
  SELECT node, hops,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6)
           AS prob
  FROM reach;
SELECT remove_provenance('btwh_tri_r2');
SELECT NOT EXISTS ((TABLE btwh_tri_r EXCEPT TABLE btwh_tri_r2)
                   UNION ALL (TABLE btwh_tri_r2 EXCEPT TABLE btwh_tri_r))
  AS hops_routes_agree;
SET provsql.provenance = 'boolean';
DROP TABLE btwh_tri_r2;
DROP TABLE btwh_tri_r;

-- The natural within-bound query deduplicates the hop column away: the
-- rewriter's plus over a vertex's per-length tokens addresses the
-- pre-created gate over the compilation's within-bound root, so every
-- root carries the d-DNNF certificate and the probability is the
-- within-4-hops reliability.  Vertices achieving a single length keep
-- that length's root (passed through the aggregation), behind its
-- 'absorptive' assumption wrapper; the certificate sits one level
-- down there.
CREATE TABLE btwh_w AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_tri e JOIN reach r ON e.src = r.node
      WHERE r.hops < 4
  )
  SELECT node FROM reach GROUP BY node;
CREATE TABLE btwh_w2 AS
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob,
         (get_infos(CASE WHEN get_gate_type(provenance()) = 'assumed'
                         THEN (get_children(provenance()))[1]
                         ELSE provenance() END)).info1 AS certified
  FROM btwh_w GROUP BY node, provenance();
SELECT remove_provenance('btwh_w2');
SELECT * FROM btwh_w2 ORDER BY node;
DROP TABLE btwh_w2;
SELECT remove_provenance('btwh_w');
DROP TABLE btwh_w;
DROP TABLE btwh_tri;

-- Undirected CASE shape, hop column first, non-zero seed, <= bound:
-- walks pump back and forth across a single edge (odd lengths to the
-- neighbour, even lengths home).
CREATE TABLE btwh_u(a int, b int);
INSERT INTO btwh_u VALUES (1,2),(2,3);
SELECT add_provenance('btwh_u');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwh_u; END $$;
CREATE TABLE btwh_u_r AS
  WITH RECURSIVE reach(hops, node) AS (
      SELECT 10, 1
    UNION
      SELECT r.hops + 1, CASE WHEN e.a = r.node THEN e.b ELSE e.a END
      FROM btwh_u e JOIN reach r ON r.node IN (e.a, e.b)
      WHERE r.hops <= 12
  )
  SELECT hops, node FROM reach;
SELECT remove_provenance('btwh_u_r');
CREATE TABLE btwh_u_p AS
  WITH RECURSIVE reach(hops, node) AS (
      SELECT 10, 1
    UNION
      SELECT r.hops + 1, CASE WHEN e.a = r.node THEN e.b ELSE e.a END
      FROM btwh_u e JOIN reach r ON r.node IN (e.a, e.b)
      WHERE r.hops <= 12
  )
  SELECT node, hops, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwh_u_p');
SELECT * FROM btwh_u_p ORDER BY node, hops;
DROP TABLE btwh_u_p;
DROP TABLE btwh_u_r;
DROP TABLE btwh_u;

-- Multi-source base arm with hops: a probabilistic source set seeds the
-- counter; source arcs contribute walk length zero.
CREATE TABLE btwh_src(v int, p float8);
INSERT INTO btwh_src VALUES (1, 0.5);
SELECT add_provenance('btwh_src');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwh_src; END $$;
CREATE TABLE btwh_m AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT v, 0 FROM btwh_src
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_edge e JOIN reach r ON e.src = r.node
      WHERE r.hops < 2
  )
  SELECT node, hops, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwh_m');
SELECT * FROM btwh_m ORDER BY node, hops;
DROP TABLE btwh_m;
DROP TABLE btwh_src;

-- A bound below the seed allows no recursive step: the base row alone.
CREATE TABLE btwh_z AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 5
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_edge e JOIN reach r ON e.src = r.node
      WHERE r.hops < 3
  )
  SELECT node, hops, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwh_z');
SELECT * FROM btwh_z;
DROP TABLE btwh_z;

-- An unbounded counter is not the shape (no fixpoint on cyclic data):
-- the generic fixpoint takes over silently on this acyclic chain.
CREATE TABLE btwh_nb AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwh_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node, hops FROM reach;
SELECT remove_provenance('btwh_nb');
SELECT count(*) AS unbounded_generic FROM btwh_nb;
DROP TABLE btwh_nb;

SET provsql.provenance = 'semiring';
DROP TABLE btwh_edge;
