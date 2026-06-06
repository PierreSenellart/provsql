\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- The user-facing bounded-treewidth reachability route: under
-- provsql.boolean_provenance, the recursive-CTE lowering recognises the
-- linear reachability shape over a tracked base edge relation and
-- compiles, along a tree decomposition of the *data* graph, one
-- certified provenance circuit per reachable vertex (deterministic /
-- decomposable by construction, linear total size for fixed data
-- treewidth, cyclic graphs native).  The resulting tokens are ordinary
-- provenance: probability_evaluate picks the linear 'independent'
-- method through the persisted d-DNNF certificate, and the whole
-- artefact surface (interpret-as-dd, Shapley) works on them.  On any
-- failure the route falls back to the generic eval_recursive fixpoint.

SET provsql.boolean_provenance = on;

-- Diamond DAG: same data and values as the recursive test (node 4:
-- 0.8018), but compiled along the data; the chooser must settle on
-- 'independent'.
CREATE TABLE btwr_edge(src int, dst int, p float8);
INSERT INTO btwr_edge VALUES
  (1,2,0.9), (1,3,0.5), (2,3,0.8), (2,4,0.6), (3,4,0.7);
SELECT add_provenance('btwr_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwr_edge; END $$;

SET provsql.last_eval_method = '';
CREATE TABLE btwr_diamond AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_diamond');
SELECT * FROM btwr_diamond ORDER BY node;
SHOW provsql.last_eval_method;
DROP TABLE btwr_diamond;

-- The tokens carry the persisted d-DNNF certificate and feed the
-- artefact surface: interpret-as-dd statistics and Shapley values
-- (edge criticality).  reach(4)'s two-edge chain through 1-2-4 and
-- 1-3-4 etc.; on a plain two-edge chain the Shapley values of the two
-- edges are equal and sum to the reliability.
CREATE TABLE btwr_chain(src int, dst int, p float8);
INSERT INTO btwr_chain VALUES (1,2,0.9), (2,3,0.8);
SELECT add_provenance('btwr_chain');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwr_chain; END $$;
CREATE TABLE btwr_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_chain e JOIN reach r ON e.src = r.node
  )
  SELECT node, provenance() pv FROM reach;
SELECT remove_provenance('btwr_r');
SELECT node, get_gate_type(pv) AS gate, (get_infos(pv)).info1 AS certified
FROM btwr_r ORDER BY node;
SELECT (ddnnf_stats((SELECT pv FROM btwr_r WHERE node = 3),
                    'interpret-as-dd')::json->>'nodes')::int > 0
  AS interpret_as_dd_works;
CREATE TABLE btwr_sh AS
  SELECT src, dst,
         round(shapley((SELECT pv FROM btwr_r WHERE node = 3),
                       provenance())::numeric, 6) AS edge_shapley
  FROM btwr_chain;
SELECT remove_provenance('btwr_sh');
SELECT * FROM btwr_sh ORDER BY src;
DROP TABLE btwr_sh;
DROP TABLE btwr_r;
DROP TABLE btwr_chain;

-- Cyclic graph: handled natively by the compilation (the fixpoint
-- driver is not involved); values as in the recursive test.
CREATE TABLE btwr_cyc(src int, dst int, p float8);
INSERT INTO btwr_cyc VALUES (1,2,0.9), (2,3,0.8), (3,2,0.5), (2,4,0.6), (3,4,0.7);
SELECT add_provenance('btwr_cyc');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwr_cyc; END $$;
SET provsql.last_eval_method = '';
CREATE TABLE btwr_cyc_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_cyc e JOIN reach r ON e.src = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS reliability
  FROM reach;
SELECT remove_provenance('btwr_cyc_r');
SELECT * FROM btwr_cyc_r ORDER BY node;
SHOW provsql.last_eval_method;
DROP TABLE btwr_cyc_r;
DROP TABLE btwr_cyc;

-- Text-valued vertices work (the route maps vertex values through text).
CREATE TABLE btwr_txt(f text, t text);
INSERT INTO btwr_txt VALUES ('a','b'), ('b','c');
SELECT add_provenance('btwr_txt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwr_txt; END $$;
CREATE TABLE btwr_txt_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 'a'::text
    UNION
      SELECT e.t FROM btwr_txt e JOIN reach r ON e.f = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_txt_r');
SELECT * FROM btwr_txt_r ORDER BY node;
DROP TABLE btwr_txt_r;
DROP TABLE btwr_txt;

-- Undirected connectivity, the natural CASE shape: the rewriter
-- recognises r.node IN (e.src, e.dst) with the matching CASE head and
-- compiles with bidirectional edges.  Undirected triangle:
-- P(a ~ c) = P(ac) + (1 - P(ac)) P(ab) P(bc) = 0.625.
CREATE TABLE btwr_tri(f text, t text);
INSERT INTO btwr_tri VALUES ('a','b'), ('b','c'), ('a','c');
SELECT add_provenance('btwr_tri');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwr_tri; END $$;
CREATE TABLE btwr_tri_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 'a'::text
    UNION
      SELECT CASE WHEN e.f = r.node THEN e.t ELSE e.f END
      FROM btwr_tri e JOIN reach r ON r.node IN (e.f, e.t)
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_tri_r');
SELECT * FROM btwr_tri_r ORDER BY node;
DROP TABLE btwr_tri_r;
DROP TABLE btwr_tri;

-- Deterministic filters over edge columns fold into the compiled route:
-- with WHERE e.w >= 5 the low-weight shortcut is excluded, so
-- reach(3) = 0.9 * 0.8 = 0.72.
CREATE TABLE btwr_w(src int, dst int, w int, p float8);
INSERT INTO btwr_w VALUES (1,2,5,0.9), (2,3,5,0.8), (1,3,1,0.5);
SELECT add_provenance('btwr_w');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwr_w; END $$;
CREATE TABLE btwr_w_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_w e JOIN reach r ON e.src = r.node
      WHERE e.w >= 5
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_w_r');
SELECT * FROM btwr_w_r ORDER BY node;
DROP TABLE btwr_w_r;
DROP TABLE btwr_w;

-- Multi-source base arm: SELECT v FROM sources.  Tracked sources form a
-- probabilistic source set (arcs from a virtual super-source gated by
-- the source tuples); untracked sources are certain.  Chain with
-- certain edges and sources {1 (p=.5), 3 (p=.5)}:
-- reach(1)=reach(2)=.5, reach(3)=reach(4)=1-(1-.5)^2=.75.
CREATE TABLE btwr_medge(src int, dst int);
INSERT INTO btwr_medge VALUES (1,2),(2,3),(3,4);
SELECT add_provenance('btwr_medge');
CREATE TABLE btwr_msrc(v int, p float8);
INSERT INTO btwr_msrc VALUES (1,0.5),(3,0.5);
SELECT add_provenance('btwr_msrc');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM btwr_msrc; END $$;
SET provsql.last_eval_method = '';
CREATE TABLE btwr_mr AS
  WITH RECURSIVE reach(node) AS (
      SELECT v FROM btwr_msrc
    UNION
      SELECT e.dst FROM btwr_medge e JOIN reach r ON e.src = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_mr');
SELECT * FROM btwr_mr ORDER BY node;
SHOW provsql.last_eval_method;
-- The same query through the generic fixpoint (GUC off) must agree.
SET provsql.boolean_provenance = off;
CREATE TABLE btwr_mr2 AS
  WITH RECURSIVE reach(node) AS (
      SELECT v FROM btwr_msrc
    UNION
      SELECT e.dst FROM btwr_medge e JOIN reach r ON e.src = r.node
  )
  SELECT node,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6)
           AS prob
  FROM reach;
SELECT remove_provenance('btwr_mr2');
SELECT NOT EXISTS ((TABLE btwr_mr EXCEPT TABLE btwr_mr2)
                   UNION ALL (TABLE btwr_mr2 EXCEPT TABLE btwr_mr))
  AS routes_agree;
SET provsql.boolean_provenance = on;
DROP TABLE btwr_mr2;
DROP TABLE btwr_mr;
-- Untracked sources are certain.
CREATE TABLE btwr_usrc(v int);
INSERT INTO btwr_usrc VALUES (2);
CREATE TABLE btwr_ur AS
  WITH RECURSIVE reach(node) AS (
      SELECT v FROM btwr_usrc
    UNION
      SELECT e.dst FROM btwr_medge e JOIN reach r ON e.src = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_ur');
SELECT * FROM btwr_ur ORDER BY node;
DROP TABLE btwr_ur;
DROP TABLE btwr_usrc;
DROP TABLE btwr_msrc;
DROP TABLE btwr_medge;

-- BID blocks: repair_key alternatives are compiled as one (k+1)-way
-- deterministic branching per block.  Node 1's outgoing edge goes to 2
-- xor to 3 (uniform repair_key block); both lead to 4:
-- reach(2)=reach(3)=0.5, reach(4)=1.
CREATE TABLE btwr_bid(src int, dst int);
INSERT INTO btwr_bid VALUES (1,2),(1,3),(2,4),(3,4);
SELECT repair_key('btwr_bid', 'src');
SET provsql.last_eval_method = '';
CREATE TABLE btwr_bid_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_bid e JOIN reach r ON e.src = r.node
  )
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob
  FROM reach;
SELECT remove_provenance('btwr_bid_r');
SELECT * FROM btwr_bid_r ORDER BY node;
-- The explicit linear method handles the mulinput literals.
CREATE TABLE btwr_bid_x AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_bid e JOIN reach r ON e.src = r.node
  )
  SELECT node, provenance() pv FROM reach;
SELECT remove_provenance('btwr_bid_x');
SELECT round(probability_evaluate(pv, 'independent')::numeric, 6)
  AS bid_independent
FROM btwr_bid_x WHERE node = 4;
DROP TABLE btwr_bid_x;
-- The generic fixpoint agrees.
SET provsql.boolean_provenance = off;
CREATE TABLE btwr_bid_r2 AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_bid e JOIN reach r ON e.src = r.node
  )
  SELECT node,
         round(probability_evaluate(provenance(),'possible-worlds')::numeric, 6)
           AS prob
  FROM reach;
SELECT remove_provenance('btwr_bid_r2');
SELECT NOT EXISTS ((TABLE btwr_bid_r EXCEPT TABLE btwr_bid_r2)
                   UNION ALL (TABLE btwr_bid_r2 EXCEPT TABLE btwr_bid_r))
  AS bid_routes_agree;
SET provsql.boolean_provenance = on;
DROP TABLE btwr_bid_r2;
DROP TABLE btwr_bid_r;
DROP TABLE btwr_bid;

-- Fallback: data treewidth above the cap (K14) falls back to the
-- generic fixpoint, with a notice under provsql.verbose_level >= 10;
-- the query still answers (acyclic orientation, so eval_recursive
-- terminates).
SET provsql.verbose_level = 10;
CREATE TABLE btwr_k14(src int, dst int);
INSERT INTO btwr_k14
  SELECT i, j FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j;
SELECT add_provenance('btwr_k14');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwr_k14; END $$;
CREATE TABLE btwr_k14_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_k14 e JOIN reach r ON e.src = r.node
  )
  SELECT node FROM reach;
SELECT remove_provenance('btwr_k14_r');
SELECT count(*) AS k14_reachable FROM btwr_k14_r;
SET provsql.verbose_level = 0;
DROP TABLE btwr_k14_r;
DROP TABLE btwr_k14;

-- Non-matching shapes keep the generic route (silently): an extra
-- filter on the recursive table...
CREATE TABLE btwr_d(src int, dst int);
INSERT INTO btwr_d VALUES (1,2),(2,3);
SELECT add_provenance('btwr_d');
CREATE TABLE btwr_d_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_d e JOIN reach r ON e.src = r.node WHERE r.node < 10
  )
  SELECT node FROM reach;
SELECT remove_provenance('btwr_d_r');
SELECT count(*) AS filtered_shape FROM btwr_d_r;
DROP TABLE btwr_d_r;
-- ... and the route is gated on boolean_provenance.
SET provsql.boolean_provenance = off;
CREATE TABLE btwr_d_r2 AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwr_d e JOIN reach r ON e.src = r.node
  )
  SELECT node FROM reach;
SELECT remove_provenance('btwr_d_r2');
SELECT count(*) AS guc_off FROM btwr_d_r2;
DROP TABLE btwr_d_r2;
SET provsql.boolean_provenance = on;
DROP TABLE btwr_d;

SET provsql.boolean_provenance = off;
DROP TABLE btwr_edge;
