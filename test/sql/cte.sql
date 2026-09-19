\set ECHO none
\pset format unaligned

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

-- CTE used inside UNION ALL branch
CREATE TABLE cte_result5 AS
  WITH eu AS (SELECT name, city FROM personnel WHERE city IN ('Paris','Berlin'))
  SELECT name, city, sr_counting(provenance(), 'personnel_count') AS counting
  FROM (
    SELECT name, city FROM eu
    UNION ALL
    SELECT name, city FROM personnel WHERE city = 'New York'
  ) t;

SELECT remove_provenance('cte_result5');
SELECT * FROM cte_result5 ORDER BY name;
DROP TABLE cte_result5;

-- Recursive CTE should error
WITH RECURSIVE nums AS (
  SELECT 1 AS n, name FROM personnel WHERE id=1
  UNION ALL
  SELECT n+1, name FROM nums WHERE n < 3
)
SELECT * FROM nums;

-- A data-modifying CTE runs once, as native SQL; its RETURNING rows carry no
-- provenance, and the rest of the query reads the data as it was before it.
CREATE TABLE cte_foo(id int PRIMARY KEY, name text UNIQUE);
INSERT INTO cte_foo VALUES (1, 'a'), (2, 'x');
SELECT add_provenance('cte_foo');
CREATE TABLE cte_ins_result AS
  WITH input(id, name) AS (VALUES (1, 'a'), (3, 'b'), (4, 'c'))
  , ins AS (
     INSERT INTO cte_foo TABLE input
     ON CONFLICT (name) DO NOTHING
     RETURNING id)
  SELECT f.id FROM input i JOIN cte_foo f USING (name)
  UNION ALL
  TABLE ins;
ALTER TABLE cte_ins_result ADD COLUMN untracked boolean;
UPDATE cte_ins_result SET untracked = (provsql = gate_one());
SELECT remove_provenance('cte_ins_result');
SELECT id, untracked FROM cte_ins_result ORDER BY id;
DROP TABLE cte_ins_result;
CREATE TABLE cte_foo_after AS SELECT id, name FROM cte_foo;
SELECT remove_provenance('cte_foo_after');
SELECT * FROM cte_foo_after ORDER BY id;
DROP TABLE cte_foo_after;

-- It cannot read a CTE that is rewritten for provenance
WITH t AS (SELECT id + 10 AS id, name || '2' AS name FROM cte_foo)
, ins AS (INSERT INTO cte_foo SELECT * FROM t RETURNING id)
SELECT * FROM t;
DROP TABLE cte_foo;

-- A CTE read from a sublink is inlined there as well (the sublink rewriting
-- then decides; before, the CTE was left unresolved).
WITH c AS (SELECT id FROM personnel WHERE city = 'Paris')
SELECT name FROM personnel WHERE id IN (SELECT id FROM c);

-- A CTE inlined in another CTE's body, itself inlined, reads a CTE kept as a
-- CTE (untracked): the reference follows it down.
CREATE TABLE cte_result_kept AS
  WITH k AS (SELECT 1 AS a UNION SELECT 2),
       r AS (SELECT city, count(*) AS n FROM personnel, k WHERE k.a = 1
             GROUP BY city),
       r2 AS (SELECT city FROM r)
  SELECT city, sr_counting(provenance(), 'personnel_count') AS counting
  FROM r2;
SELECT remove_provenance('cte_result_kept');
SELECT * FROM cte_result_kept ORDER BY city;
DROP TABLE cte_result_kept;
