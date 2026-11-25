\set ECHO none
\pset format unaligned

-- The provenance of a condition on aggregates is the sum of the annotations
-- of the worlds where it holds, the worlds of all the rows it reads, in
-- every semiring, which only a semiring that is not a Boolean algebra tells
-- apart from the shortcut it replaces.
--
--  * A row an aggregate reads no value from -- a NULL argument of sum or min,
--    a row its FILTER leaves out -- is one of those rows: with a row a of
--    value 50 and a row b of value NULL, min < 60 holds in {a} and {a, b},
--    whose annotations sum to a ⊗ (1 ⊖ b) ⊕ a ⊗ b, and not to a.
--
-- Rows are annotated in the counting semiring by their column w (2 and 3),
-- where 1 ⊖ 3 = 0: a ⊗ (1 ⊖ b) ⊕ a ⊗ b is 2 ⊗ 3 = 6 and a is 2.  In the
-- Boolean semiring both forms coincide, and the answers there are unchanged.

CREATE TABLE hjw_emp(id int, dept int, sal int, w int);
INSERT INTO hjw_emp VALUES
  (1, 1, 50, 2), (2, 1, NULL, 3),   -- dept 1: a value and a NULL
  (3, 2, 50, 2), (4, 2, 70, 3);     -- dept 2: two values
SELECT add_provenance('hjw_emp');
SELECT create_provenance_mapping('hjw_emp_w', 'hjw_emp', 'w');

-- min < 60 holds in {a} and {a, b}, in both departments: 6 and 6.
CREATE TABLE hjw_r AS
  SELECT dept, sr_counting(provenance(), 'hjw_emp_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_emp GROUP BY dept HAVING min(sal) < 60;
SELECT remove_provenance('hjw_r');
SELECT 'min < 60' AS q, dept, n, b FROM hjw_r ORDER BY dept;
DROP TABLE hjw_r;

-- sum < 60 holds in {a} and {a, b} in dept 1 (6), in {a} alone in dept 2,
-- 2 ⊗ (1 ⊖ 3) = 0.
CREATE TABLE hjw_r AS
  SELECT dept, sr_counting(provenance(), 'hjw_emp_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_emp GROUP BY dept HAVING sum(sal) < 60;
SELECT remove_provenance('hjw_r');
SELECT 'sum < 60' AS q, dept, n, b FROM hjw_r ORDER BY dept;
DROP TABLE hjw_r;

-- A FILTER leaving out the row of value 70 makes dept 2 the same as dept 1:
-- 6 and 6.
CREATE TABLE hjw_r AS
  SELECT dept, sr_counting(provenance(), 'hjw_emp_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_emp GROUP BY dept HAVING min(sal) FILTER (WHERE id <> 4) < 60;
SELECT remove_provenance('hjw_r');
SELECT 'min filter < 60' AS q, dept, n, b FROM hjw_r ORDER BY dept;
DROP TABLE hjw_r;

-- The same condition as min < 60, spelled with a count: the same answer.
CREATE TABLE hjw_r AS
  SELECT dept, sr_counting(provenance(), 'hjw_emp_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_emp GROUP BY dept HAVING count(*) FILTER (WHERE sal < 60) >= 1;
SELECT remove_provenance('hjw_r');
SELECT 'count filter >= 1' AS q, dept, n, b FROM hjw_r ORDER BY dept;
DROP TABLE hjw_r;

DROP TABLE hjw_emp_w;
DROP TABLE hjw_emp;
