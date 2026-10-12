\set ECHO none
\pset format unaligned

-- Non-commutative semirings: a product gate keeps its factors in the order
-- of the query, and the regular expressions of sr_regex concatenate them in
-- that order.  On a recursion, the equations keep the recursive relation on
-- its side of each coefficient (sr_formula prints them so), and Gaussian
-- elimination solves them in either order, the star on the matching side;
-- a recursion with the recursive relation between two factors is refused.
--
-- The graph: a -ab-> b -bc-> c -ca-> a, and a -ac-> c.

CREATE TABLE nc_e(src text, dst text, lbl text, cost float8);
INSERT INTO nc_e VALUES
  ('a', 'b', 'ab', 1), ('b', 'c', 'bc', 2), ('c', 'a', 'ca', 0),
  ('a', 'c', 'ac', 3);
SELECT add_provenance('nc_e');
SELECT create_provenance_mapping('nc_lbl', 'nc_e', 'lbl');
SELECT create_provenance_mapping('nc_cost', 'nc_e', 'cost');

-- Without recursion: the words of a join, in FROM order.
CREATE TABLE nc_res AS
  SELECT e1.src, e2.dst, sr_regex(provenance(), 'nc_lbl') AS word
  FROM nc_e e1 JOIN nc_e e2 ON e1.dst = e2.src;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res ORDER BY src, dst;
DROP TABLE nc_res;

-- The recursive relation first: paths in the order they are followed.
CREATE TABLE nc_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM r JOIN nc_e e ON e.src = r.n)
  SELECT n, sr_formula(provenance(), 'nc_lbl') AS equations,
         sr_regex(provenance(), 'nc_lbl') AS paths
  FROM r;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res ORDER BY n;
DROP TABLE nc_res;

-- The recursive relation last: the same paths, read backwards.
CREATE TABLE nc_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM nc_e e JOIN r ON e.src = r.n)
  SELECT n, sr_formula(provenance(), 'nc_lbl') AS equations,
         sr_regex(provenance(), 'nc_lbl') AS paths
  FROM r;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res ORDER BY n;
DROP TABLE nc_res;

-- Least cost, with the paths of that cost: c is reached at cost 3 directly
-- or through b.
CREATE TABLE nc_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM r JOIN nc_e e ON e.src = r.n)
  SELECT n, (sr_tropical_paths(provenance(), 'nc_cost', 'nc_lbl')).*
  FROM r;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res ORDER BY n;
DROP TABLE nc_res;

-- A cycle of cost zero can be followed any number of times on a shortest
-- path: it comes under a star.
UPDATE nc_cost SET value = 0;
CREATE TABLE nc_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e.dst FROM r JOIN nc_e e ON e.src = r.n)
  SELECT n, (sr_tropical_paths(provenance(), 'nc_cost', 'nc_lbl')).*
  FROM r;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res ORDER BY n;
DROP TABLE nc_res;

-- A cycle of negative cost has no shortest path.
UPDATE nc_cost SET value = -1
  WHERE provenance = (SELECT provsql FROM nc_e WHERE lbl = 'ca');
WITH RECURSIVE r(n) AS (
    SELECT 'a'::text
  UNION
    SELECT e.dst FROM r JOIN nc_e e ON e.src = r.n)
SELECT n, (sr_tropical_paths(provenance(), 'nc_cost', 'nc_lbl')).cost
FROM r WHERE n = 'b';

-- The recursive relation between two factors: refused by sr_regex, while
-- sr_formula prints the equations, factors on both sides.
WITH RECURSIVE r(n) AS (
    SELECT 'a'::text
  UNION
    SELECT e2.dst FROM nc_e e1 JOIN r ON e1.dst = r.n
                   JOIN nc_e e2 ON e2.src = r.n)
SELECT n, sr_regex(provenance(), 'nc_lbl') FROM r WHERE n = 'b';
CREATE TABLE nc_res AS
  WITH RECURSIVE r(n) AS (
      SELECT 'a'::text
    UNION
      SELECT e2.dst FROM nc_e e1 JOIN r ON e1.dst = r.n
                     JOIN nc_e e2 ON e2.src = r.n)
  SELECT n, sr_formula(provenance(), 'nc_lbl') AS equations FROM r;
SELECT remove_provenance('nc_res');
SELECT * FROM nc_res WHERE n = 'b';
DROP TABLE nc_res;

-- HAVING: the provenance of a condition on aggregates is a product over the
-- worlds of the group, in no order of the query's; refused.
SELECT src, sr_regex(provenance(), 'nc_lbl')
FROM nc_e GROUP BY src HAVING count(*) > 1;

SELECT remove_provenance('nc_e');
DROP TABLE nc_e, nc_lbl, nc_cost;
