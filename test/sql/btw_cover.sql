\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Reachability self-join conjunctions: "are these k vertices all
-- reachable" -- FROM reach r1, ..., reach rk with one constant node
-- binding each.  The row provenance is the provenance_times of
-- *correlated* per-vertex reach tokens (they share edges), which
-- entangles the per-vertex certificates.  The lowering detects the
-- shape, compiles the certified all-members-reachable circuit (the
-- pending rescuer-set congruence folded through the decomposition DP)
-- and plants it at the times-canonical address the conjunction
-- computes -- so the natural query stays on the linear certified
-- route, with joint-worlds semantics: probability evaluation is the
-- k-terminal reliability, nonnegative min-plus the cost of the
-- cheapest covering subgraph (directed Steiner cost), shared edges
-- paid once.

SET provsql.provenance = 'absorptive';

-- Hub network: 1->2 (cost 1), 1->3 (4), 2->4 (2), 3->4 (1),
-- 4->5 (3), 4->6 (2); p = .9 everywhere.
CREATE TABLE btwc_link(src int, dst int, cost float8);
INSERT INTO btwc_link VALUES
  (1,2,1),(1,3,4),(2,4,2),(3,4,1),(4,5,3),(4,6,2);
SELECT add_provenance('btwc_link');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.9) FROM btwc_link; END $$;
SELECT create_provenance_mapping('btwc_cost', 'btwc_link', 'cost');

-- A conjunction outside the recognised shape first (the second
-- binding is an expression): nothing is planted, the generic path
-- stays correct -- and uncertified.
CREATE TABLE btwc_fb AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwc_link e JOIN reach r ON e.src = r.node
  )
  SELECT 1 AS ok, provenance() AS pv
  FROM reach r1, reach r2
  WHERE r1.node = 5 AND r2.node = r1.node + 1;
SELECT remove_provenance('btwc_fb');
-- P(reach 5 and reach 6) = P(reach 4)*.9*.9
--   = (.81 + .81 - .81^2)*.81 = .780759.
SELECT (get_infos(pv)).info1 AS certified,
       round(probability_evaluate(pv, 'possible-worlds')::numeric, 6) AS rel
FROM btwc_fb;
DROP TABLE btwc_fb;

-- The recognised shape: the certified all-members circuit is planted
-- at the times-canonical address; probability evaluates through the
-- linear independent route, exactly.
CREATE TABLE btwc_2 AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwc_link e JOIN reach r ON e.src = r.node
  )
  SELECT 'both supplied' AS answer, provenance() AS pv
  FROM reach r1, reach r2
  WHERE r1.node = 5 AND r2.node = 6;
SELECT remove_provenance('btwc_2');
-- Steiner cost: trunk 1->2->4 (3) + 4->5 (3) + 4->6 (2) = 8, the
-- shared trunk paid once (the raw product would pay it twice).
SELECT answer, get_gate_type(pv) AS root, (get_infos(pv)).info1 AS certified,
       round(probability_evaluate(pv, 'independent')::numeric, 6) AS rel,
       round(probability_evaluate(pv, 'possible-worlds')::numeric, 6) AS rel_pw,
       sr_tropical(pv, 'btwc_cost', nonnegative => true) AS steiner
FROM btwc_2;
DROP TABLE btwc_2;

-- The deviating phrasing now computes the same token multiset and
-- lands on the planted gate: content addressing, not syntax.
CREATE TABLE btwc_fb2 AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwc_link e JOIN reach r ON e.src = r.node
  )
  SELECT 1 AS ok, provenance() AS pv
  FROM reach r1, reach r2
  WHERE r1.node = 5 AND r2.node = r1.node + 1;
SELECT remove_provenance('btwc_fb2');
SELECT (get_infos(pv)).info1 AS certified,
       round(probability_evaluate(pv, 'independent')::numeric, 6) AS rel
FROM btwc_fb2;
DROP TABLE btwc_fb2;

-- Three terminals; the optimal covering tree routes *through*
-- terminal 3: 1->3 (4) + 3->4 (1) + 4->5 (3) + 4->6 (2) = 10.
CREATE TABLE btwc_3 AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwc_link e JOIN reach r ON e.src = r.node
  )
  SELECT 1 AS ok, provenance() AS pv
  FROM reach r1, reach r2, reach r3
  WHERE r1.node = 5 AND r2.node = 6 AND r3.node = 3;
SELECT remove_provenance('btwc_3');
SELECT (get_infos(pv)).info1 AS certified,
       round(probability_evaluate(pv, 'independent')::numeric, 6) AS rel,
       round(probability_evaluate(pv, 'possible-worlds')::numeric, 6) AS rel_pw,
       sr_tropical(pv, 'btwc_cost', nonnegative => true) AS steiner
FROM btwc_3;
-- Counting refuses the planted token (absorptive quotient only).
SELECT sr_counting(pv, 'btwc_cost') FROM btwc_3;
DROP TABLE btwc_3;

-- Cyclic data: a return edge 6->1 changes nothing for the terminals
-- (loops never help coverage), and the compilation handles the cycle
-- natively.
CREATE TABLE btwc_cyc(src int, dst int, cost float8);
INSERT INTO btwc_cyc VALUES
  (1,2,1),(2,3,4),(3,1,1),(2,4,2);
SELECT add_provenance('btwc_cyc');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwc_cyc; END $$;
SELECT create_provenance_mapping('btwc_cyc_cost', 'btwc_cyc', 'cost');
CREATE TABLE btwc_c AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwc_cyc e JOIN reach r ON e.src = r.node
  )
  SELECT 1 AS ok, provenance() AS pv
  FROM reach r1, reach r2
  WHERE r1.node = 3 AND r2.node = 4;
SELECT remove_provenance('btwc_c');
-- Both need 1->2 (.5), then 2->3 (.5) and 2->4 (.5): rel = .125;
-- Steiner = 1 + 4 + 2 = 7.
SELECT (get_infos(pv)).info1 AS certified,
       round(probability_evaluate(pv, 'independent')::numeric, 6) AS rel,
       round(probability_evaluate(pv, 'possible-worlds')::numeric, 6) AS rel_pw,
       sr_tropical(pv, 'btwc_cyc_cost', nonnegative => true) AS steiner
FROM btwc_c;
DROP TABLE btwc_c;
SELECT remove_provenance('btwc_cyc');
DROP TABLE btwc_cyc;

SET provsql.provenance = 'semiring';
SELECT remove_provenance('btwc_link');
DROP TABLE btwc_link;
RESET provsql.provenance;
