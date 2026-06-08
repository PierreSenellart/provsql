\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Cross-vertex reachability aggregations: GROUP BY over a join of a
-- reachability CTE with an untracked member relation collapses each
-- group's per-vertex reach tokens into one OR of *correlated* events
-- (the vertices share edges), which no per-vertex certificate covers.
-- The lowering detects the shape, compiles per group the certified
-- "some member reachable" circuit (the set-reachability bit folded
-- through the decomposition DP), and plants it at the canonical
-- address the aggregation will compute -- so the natural query stays
-- on the linear certified route.

SET provsql.provenance = 'boolean';

-- Diamond 1->{2,3}->4 with tail 4->5, p=.5; regions A={2,3}, B={4,5}.
-- P(A) = 1-(1-.5)^2 = .75; P(B) = P(reach 4) = 1-(1-.25)^2 = .4375
-- (5 is only reachable through 4).
CREATE TABLE btwa_edge(src int, dst int);
INSERT INTO btwa_edge VALUES (1,2),(1,3),(2,4),(3,4),(4,5);
SELECT add_provenance('btwa_edge');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwa_edge; END $$;
CREATE TABLE btwa_regions(node int, region text);
INSERT INTO btwa_regions VALUES (2,'A'),(3,'A'),(4,'B'),(5,'B');

CREATE TABLE btwa_r AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwa_edge e JOIN reach r ON e.src = r.node
  )
  SELECT t.region FROM reach r JOIN btwa_regions t ON r.node = t.node
  GROUP BY t.region;
CREATE TABLE btwa_p AS
  SELECT region,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS p_independent,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds,
         (get_infos(provenance())).info1 AS certified
  FROM btwa_r GROUP BY region, provenance();
SELECT remove_provenance('btwa_p');
SELECT * FROM btwa_p ORDER BY region;
DROP TABLE btwa_p;
SELECT remove_provenance('btwa_r');
DROP TABLE btwa_r;

-- SELECT DISTINCT is provenance-identical to GROUP BY: normalised
-- before CTE lowering, so the same certified any-member gate is
-- planted and the same per-region reliability evaluates through the
-- linear route (certified, matching the GROUP BY case exactly).
CREATE TABLE btwa_rd AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwa_edge e JOIN reach r ON e.src = r.node
  )
  SELECT DISTINCT t.region FROM reach r JOIN btwa_regions t ON r.node = t.node;
CREATE TABLE btwa_pd AS
  SELECT region,
         round(probability_evaluate(provenance(), 'independent')::numeric, 6)
           AS p_independent,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds,
         (get_infos(provenance())).info1 AS certified
  FROM btwa_rd GROUP BY region, provenance();
SELECT remove_provenance('btwa_pd');
SELECT * FROM btwa_pd ORDER BY region;
DROP TABLE btwa_pd;
SELECT remove_provenance('btwa_rd');
DROP TABLE btwa_rd;

-- A *tracked* member relation makes the aggregated tokens per-row
-- products: nothing is planted, the generic path stays correct.
CREATE TABLE btwa_tracked(node int, region text);
INSERT INTO btwa_tracked VALUES (2,'A'),(3,'A');
SELECT add_provenance('btwa_tracked');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM btwa_tracked; END $$;
CREATE TABLE btwa_tr AS
  WITH RECURSIVE reach(node) AS (
      SELECT 1
    UNION
      SELECT e.dst FROM btwa_edge e JOIN reach r ON e.src = r.node
  )
  SELECT t.region FROM reach r JOIN btwa_tracked t ON r.node = t.node
  GROUP BY t.region;
CREATE TABLE btwa_tp AS
  SELECT region,
         round(probability_evaluate(provenance(), 'possible-worlds')::numeric, 6)
           AS p_worlds
  FROM btwa_tr GROUP BY region, provenance();
SELECT remove_provenance('btwa_tp');
-- P = 1 - (1 - .5*.5)^2 = .4375 (per-vertex reach times region-row).
SELECT * FROM btwa_tp ORDER BY region;
DROP TABLE btwa_tp;
SELECT remove_provenance('btwa_tr');
DROP TABLE btwa_tr;
SELECT remove_provenance('btwa_tracked');
DROP TABLE btwa_tracked;

SET provsql.provenance = 'semiring';
SELECT remove_provenance('btwa_edge');
DROP TABLE btwa_edge;
DROP TABLE btwa_regions;
RESET provsql.provenance;
