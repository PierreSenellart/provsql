\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Single input leaf: no children, only the root row at depth 0.
CREATE TABLE cs_q AS SELECT provenance() AS p FROM personnel WHERE name='John';
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, parent, child_pos
FROM cs_q, LATERAL provsql.circuit_subgraph(p);
DROP TABLE cs_q;

-- SELECT DISTINCT 1 collapses 7 personnel rows into one plus over 7 inputs.
CREATE TABLE cs_q AS
  SELECT provenance() AS p FROM (SELECT DISTINCT 1 FROM personnel) t;
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
DROP TABLE cs_q;

-- Self-join: times of two input gates.
CREATE TABLE cs_q AS
  SELECT provenance() AS p
  FROM personnel p1, personnel p2
  WHERE p1.id=1 AND p2.id=2;
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;

-- BFS invariants on the same circuit: every non-root edge's parent is
-- present in the result, and parent.depth + 1 >= child.depth (equality
-- on shortest-path edges, strict inequality on shortcut edges into a
-- multi-parent child). The self-join here is a tree, so equality holds
-- everywhere — multi-parent inputs would relax the second column.
WITH cs AS (
  SELECT s.* FROM cs_q, LATERAL provsql.circuit_subgraph(p) s
)
SELECT
  bool_and(p.node IS NOT NULL) AS all_parents_present,
  bool_and(p.depth + 1 >= c.depth) AS depth_chain_consistent
FROM cs c
LEFT JOIN cs p ON c.parent = p.node
WHERE c.parent IS NOT NULL;
DROP TABLE cs_q;

-- UNION over name with a single-name selection on top: one plus over two inputs.
CREATE TABLE cs_q AS
  SELECT provenance() AS p
  FROM (SELECT name FROM personnel
        UNION
        SELECT name FROM personnel) t
  WHERE name='John';
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
DROP TABLE cs_q;

-- GROUP BY: an agg gate over the personnel rows of the relevant city.
CREATE TABLE cs_q AS
  SELECT provenance() AS p
  FROM (SELECT city, count(*) FROM personnel WHERE city='Paris' GROUP BY city) t;
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
DROP TABLE cs_q;

-- Depth limit: max_depth=0 returns only the root; max_depth=1 returns root + first layer.
CREATE TABLE cs_q AS
  SELECT provenance() AS p FROM (SELECT DISTINCT 1 FROM personnel) t;
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p, 0)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p, 1)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
DROP TABLE cs_q;

-- where_provenance=on adds project and eq gates around inputs / joins.
SET provsql.where_provenance = on;

CREATE TABLE cs_q AS SELECT provenance() AS p FROM personnel WHERE name='John';
SELECT remove_provenance('cs_q');
SELECT gate_type, depth, COUNT(*) AS n
FROM cs_q, LATERAL provsql.circuit_subgraph(p)
GROUP BY gate_type, depth
ORDER BY depth, gate_type;
DROP TABLE cs_q;

SET provsql.where_provenance = off;
