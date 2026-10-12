\set ECHO none
\pset format unaligned

-- sr_best_path: one derivation of least cost, the one with the fewest
-- labels, then the first in lexicographic order of its labels; labels in
-- the order of the query.  A chain, absorptive on both sides of a product,
-- with a monus: a recursion may have the recursive relation anywhere, and
-- outer joins and EXCEPT are evaluated.
--
-- The graph: a -ab(1)-> b -bc(2)-> c -ca(0)-> a, and a -ac(4)-> c.

CREATE TABLE bp_e(src text, dst text, lbl text, cost float8);
INSERT INTO bp_e VALUES
  ('a', 'b', 'ab', 1), ('b', 'c', 'bc', 2), ('c', 'a', 'ca', 0),
  ('a', 'c', 'ac', 4);
SELECT add_provenance('bp_e');
SELECT create_provenance_mapping('bp_lbl', 'bp_e', 'lbl');
SELECT create_provenance_mapping('bp_cost', 'bp_e', 'cost');

-- A join: the labels in FROM order.
CREATE TABLE bp_res AS
  SELECT e1.src, e2.dst, (sr_best_path(provenance(), 'bp_cost', 'bp_lbl')).*
  FROM bp_e e1 JOIN bp_e e2 ON e1.dst = e2.src;
SELECT remove_provenance('bp_res');
SELECT src, dst, cost, path FROM bp_res ORDER BY src, dst;
DROP TABLE bp_res;

-- The recursive relation first: c through b (cost 3), not directly (4).
CREATE TABLE bp_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM r JOIN bp_e e ON e.src = r.n)
  SELECT n, (sr_best_path(provenance(), 'bp_cost', 'bp_lbl')).*
  FROM r;
SELECT remove_provenance('bp_res');
SELECT n, cost, path FROM bp_res ORDER BY n;
DROP TABLE bp_res;

-- The recursive relation last: the same path, read backwards.
CREATE TABLE bp_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM bp_e e JOIN r ON e.src = r.n)
  SELECT n, (sr_best_path(provenance(), 'bp_cost', 'bp_lbl')).*
  FROM r;
SELECT remove_provenance('bp_res');
SELECT n, cost, path FROM bp_res ORDER BY n;
DROP TABLE bp_res;

-- The recursive relation between two factors, which sr_regex refuses:
-- every star is 1 here.  b is reached by ca, then a, then ab.
CREATE TABLE bp_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e2.dst FROM bp_e e1 JOIN r ON e1.dst = r.n
                     JOIN bp_e e2 ON e2.src = r.n)
  SELECT n, (sr_best_path(provenance(), 'bp_cost', 'bp_lbl')).*
  FROM r;
SELECT remove_provenance('bp_res');
SELECT n, cost, path FROM bp_res ORDER BY n;
DROP TABLE bp_res;

-- At equal costs, the fewest labels, then the lexicographic order: c at
-- cost 3 directly (ac), rather than through b.
INSERT INTO bp_e VALUES ('a', 'd', 'x2', 1), ('a', 'd', 'x1', 1);
CREATE TABLE bp_lbl2 AS SELECT provsql AS provenance, lbl AS value FROM bp_e;
CREATE TABLE bp_cost2 AS
  SELECT provsql AS provenance,
         CASE lbl WHEN 'ac' THEN 3 ELSE cost END AS value FROM bp_e;
SELECT remove_provenance('bp_lbl2');
SELECT remove_provenance('bp_cost2');
CREATE TABLE bp_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM r JOIN bp_e e ON e.src = r.n)
  SELECT n, (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).*
  FROM r;
SELECT remove_provenance('bp_res');
SELECT n, cost, path FROM bp_res ORDER BY n;
DROP TABLE bp_res;

-- The monus: a value minus one at least as good is 0, the value otherwise.
-- The destinations that are no source: d always; a (ca, cost 0) unless a
-- source a at least as good, and the best one costs 1 (ab); c (bc, cost 2)
-- never, the source c costing 0; with no derivation, an infinite cost and
-- no path.
CREATE TABLE bp_res AS
  SELECT dst, (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).*
  FROM (SELECT dst FROM bp_e EXCEPT SELECT src FROM bp_e) t;
SELECT remove_provenance('bp_res');
SELECT dst, cost, path FROM bp_res ORDER BY dst;
DROP TABLE bp_res;

-- An outer join: ab with its continuation bc, and the row of ab padded.
CREATE TABLE bp_res AS
  SELECT e1.lbl AS l1, e2.lbl AS l2,
         (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).*
  FROM bp_e e1 LEFT JOIN bp_e e2 ON e1.dst = e2.src AND e2.lbl = 'bc'
  WHERE e1.lbl = 'ab';
SELECT remove_provenance('bp_res');
SELECT l1, l2, cost, path FROM bp_res ORDER BY l2;
DROP TABLE bp_res;

-- Refused: HAVING (no order of the query on the worlds of a group), the
-- 'absorptive' provenance class (its circuits do not keep the order of the
-- query), and a negative cost.
SELECT src, (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).cost
FROM bp_e GROUP BY src HAVING count(*) > 1;
SET provsql.provenance = 'absorptive';
WITH RECURSIVE r(n) AS (
    SELECT 'a'::text
  UNION
    SELECT e.dst FROM r JOIN bp_e e ON e.src = r.n)
SELECT n, (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).cost
FROM r WHERE n = 'c';
RESET provsql.provenance;
UPDATE bp_cost2 SET value = -1 WHERE value = 0;
SELECT (sr_best_path(provenance(), 'bp_cost2', 'bp_lbl2')).cost
FROM bp_e WHERE lbl = 'ca';

SELECT remove_provenance('bp_e');
DROP TABLE bp_e, bp_lbl, bp_cost, bp_lbl2, bp_cost2;
