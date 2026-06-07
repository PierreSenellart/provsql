\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Min-cost reachability through the decomposition-aligned compilation:
-- under the 'absorptive' provenance class the reachability route's
-- certified d-DNNF tokens evaluate exactly in any absorptive semiring
-- (the deterministic world enumeration surfaces every minimal
-- derivation support -- here, every path -- and absorption erases the
-- rest), so sr_tropical(..., nonnegative => true) computes
-- single-source min-cost in time linear in the circuit, cyclic data
-- included.  No dedicated evaluation rule: the certificate's
-- monus(one, edge) negations natively evaluate to the tropical one on
-- positively-priced edges, and the worlds a free (cost-0) edge's
-- negation kills are dominated by their supersets at equal cost.

SET provsql.provenance = 'absorptive';

-- Cost-0 edge: 1->2 free, 2->3 costs 5, direct 1->3 costs 6, free
-- cycle edge 3->1.  Min cost to 3 is 5, through the free edge.
CREATE TABLE btwt_edge(src int, dst int, cost float8);
INSERT INTO btwt_edge VALUES (1,2,0),(2,3,5),(1,3,6),(3,1,0);
SELECT add_provenance('btwt_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwt_edge; END $$;
SELECT create_provenance_mapping('btwt_cost', 'btwt_edge', 'cost');
CREATE TABLE btwt_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwt_edge e JOIN reach r ON e.src = r.node
  )
  SELECT node, provenance() pv FROM reach;
SELECT remove_provenance('btwt_r');
SELECT node, sr_tropical(pv, 'btwt_cost', nonnegative => true) AS min_cost
FROM btwt_r ORDER BY node;
DROP TABLE btwt_r;
DROP TABLE btwt_edge;

-- Hop-bounded min-cost (constrained shortest path): unit-cost chain
-- 1->2->3->4->5 against a 1-hop shortcut 1->5 of cost 10.  Within 2
-- hops the cheap chain is out of reach: min cost to 5 is 10, and node
-- 4 (3 hops away) is not reported at all.
CREATE TABLE btwt_chain(src int, dst int, cost float8);
INSERT INTO btwt_chain VALUES (1,2,1),(2,3,1),(3,4,1),(4,5,1),(1,5,10);
SELECT add_provenance('btwt_chain');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwt_chain; END $$;
SELECT create_provenance_mapping('btwt_chain_cost', 'btwt_chain', 'cost');
CREATE TABLE btwt_h AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwt_chain e JOIN reach r ON e.src = r.node
      WHERE r.hops < 2
  )
  SELECT node FROM reach GROUP BY node;
CREATE TABLE btwt_h2 AS
  SELECT node, sr_tropical(provenance(), 'btwt_chain_cost',
                           nonnegative => true) AS min_cost_2hops
  FROM btwt_h GROUP BY node, provenance();
SELECT remove_provenance('btwt_h2');
SELECT * FROM btwt_h2 ORDER BY node;
DROP TABLE btwt_h2;
SELECT remove_provenance('btwt_h');
DROP TABLE btwt_h;
DROP TABLE btwt_chain;

-- Multi-length vertices land on the planted certified canonical:
-- cyclic triangle 1->2 (cost 3), 2->3 (4), 3->1 (1), within 4 hops --
-- nodes 1 and 2 achieve two walk lengths each (the cycle pumps
-- lengths), node 3 a single one.  Reliability and min-cost evaluate
-- on the same tokens.
CREATE TABLE btwt_tri(src int, dst int, cost float8);
INSERT INTO btwt_tri VALUES (1,2,3),(2,3,4),(3,1,1);
SELECT add_provenance('btwt_tri');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwt_tri; END $$;
SELECT create_provenance_mapping('btwt_tri_cost', 'btwt_tri', 'cost');
CREATE TABLE btwt_t AS
  WITH RECURSIVE reach(node, hops) AS (
      SELECT 1, 0
    UNION
      SELECT e.dst, r.hops + 1 FROM btwt_tri e JOIN reach r ON e.src = r.node
      WHERE r.hops < 4
  )
  SELECT node FROM reach GROUP BY node;
CREATE TABLE btwt_t2 AS
  SELECT node, round(probability_evaluate(provenance())::numeric, 6) AS prob,
         sr_tropical(provenance(), 'btwt_tri_cost', nonnegative => true)
           AS min_cost,
         get_gate_type(provenance()) AS root_type
  FROM btwt_t GROUP BY node, provenance();
SELECT remove_provenance('btwt_t2');
SELECT * FROM btwt_t2 ORDER BY node;
DROP TABLE btwt_t2;
SELECT remove_provenance('btwt_t');
DROP TABLE btwt_t;
DROP TABLE btwt_tri;

-- Cross-vertex region min-cost: the GROUP BY over a join with an
-- untracked member relation lands on the planted any-member circuit,
-- giving the min cost of reaching each region.  Diamond
-- 1->{2,3}->4->5: region A = {2,3} costs min(5,2) = 2, region B =
-- {4,5} costs min(5+1, 2+9) = 6.
CREATE TABLE btwt_dia(src int, dst int, cost float8);
INSERT INTO btwt_dia VALUES (1,2,5),(1,3,2),(2,4,1),(3,4,9),(4,5,1);
SELECT add_provenance('btwt_dia');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwt_dia; END $$;
SELECT create_provenance_mapping('btwt_dia_cost', 'btwt_dia', 'cost');
CREATE TABLE btwt_regions(node int, region text);
INSERT INTO btwt_regions VALUES (2,'A'),(3,'A'),(4,'B'),(5,'B');
CREATE TABLE btwt_g AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwt_dia e JOIN reach r ON e.src = r.node
  )
  SELECT t.region FROM reach r JOIN btwt_regions t ON r.node = t.node
  GROUP BY t.region;
CREATE TABLE btwt_g2 AS
  SELECT region,
         sr_tropical(provenance(), 'btwt_dia_cost', nonnegative => true)
           AS min_cost,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS prob,
         (get_infos(provenance())).info1 AS certified
  FROM btwt_g GROUP BY region, provenance();
SELECT remove_provenance('btwt_g2');
SELECT * FROM btwt_g2 ORDER BY region;
DROP TABLE btwt_g2;
-- Counting refuses the routed tokens, through the planted canonical
-- as well: the compiled circuit only represents the absorptive
-- quotient of the recursive provenance.
SELECT sr_counting(provenance(), 'btwt_dia_cost') FROM btwt_g
GROUP BY region, provenance();
SELECT remove_provenance('btwt_g');
DROP TABLE btwt_g;
DROP TABLE btwt_regions;
DROP TABLE btwt_dia;

SET provsql.provenance = 'semiring';
RESET provsql.provenance;
