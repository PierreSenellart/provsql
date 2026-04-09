\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Basic CTE with provenance
CREATE TABLE cte_result1 AS
  WITH paris AS (SELECT name, city FROM personnel WHERE city='Paris')
  SELECT name, sr_counting(provenance(), 'personnel_count') AS counting
  FROM paris;

SELECT remove_provenance('cte_result1');
SELECT * FROM cte_result1 ORDER BY name;
DROP TABLE cte_result1;

-- Multiple CTEs joined
CREATE TABLE cte_result2 AS
  WITH paris AS (SELECT * FROM personnel WHERE city='Paris'),
       berlin AS (SELECT * FROM personnel WHERE city='Berlin')
  SELECT p.name AS p_name, b.name AS b_name,
         sr_counting(provenance(), 'personnel_count') AS counting
  FROM paris p, berlin b;

SELECT remove_provenance('cte_result2');
SELECT * FROM cte_result2 ORDER BY p_name, b_name;
DROP TABLE cte_result2;

-- CTE referenced twice
CREATE TABLE cte_result3 AS
  WITH eu AS (SELECT name, city FROM personnel WHERE city IN ('Paris','Berlin'))
  SELECT e1.name AS name1, e2.name AS name2,
         sr_counting(provenance(), 'personnel_count') AS counting
  FROM eu e1 JOIN eu e2 ON e1.city = e2.city AND e1.name < e2.name;

SELECT remove_provenance('cte_result3');
SELECT * FROM cte_result3 ORDER BY name1, name2;
DROP TABLE cte_result3;

-- Nested CTEs (b references a, c references b)
CREATE TABLE cte_result4 AS
  WITH a AS (SELECT * FROM personnel WHERE city='Paris'),
       b AS (SELECT name, id FROM a),
       c AS (SELECT name FROM b WHERE id > 4)
  SELECT name, sr_counting(provenance(), 'personnel_count') AS counting
  FROM c;

SELECT remove_provenance('cte_result4');
SELECT * FROM cte_result4 ORDER BY name;
DROP TABLE cte_result4;

-- Recursive CTE should error
WITH RECURSIVE nums AS (
  SELECT 1 AS n, name FROM personnel WHERE id=1
  UNION ALL
  SELECT n+1, name FROM nums WHERE n < 3
)
SELECT * FROM nums;
