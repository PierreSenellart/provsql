\set ECHO none
\pset format unaligned

-- The provenance of a condition on aggregates is the sum of the annotations
-- of the worlds where it holds, the worlds of all the rows it reads, in
-- every semiring: two consequences that only a semiring which is not a
-- Boolean algebra tells apart from the shortcuts they replace.
--
--  * A row an aggregate reads no value from -- a NULL argument of sum or min,
--    a row its FILTER leaves out -- is one of those rows: with a row a of
--    value 50 and a row b of value NULL, min < 60 holds in {a} and {a, b},
--    whose annotations sum to a ⊗ (1 ⊖ b) ⊕ a ⊗ b, and not to a.
--  * A disjunction with a regular condition that holds -- name = 30 OR
--    EXISTS (...) -- holds in every world of the rows the EXISTS reads: over
--    one matching row e, (1 ⊖ e) ⊕ e, and not 1 ⊕ e.
--
-- Rows are annotated in the counting semiring by their column w (2 and 3),
-- where 1 ⊖ 3 = 0: a ⊗ (1 ⊖ b) ⊕ a ⊗ b is 2 ⊗ 3 = 6 and a is 2; (1 ⊖ e) ⊕ e
-- is 3 and 1 ⊕ e is 4.  In the Boolean semiring both forms coincide, and the
-- answers there are unchanged.

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

-- The same over other aggregates: bool_and over (true, NULL), min over
-- texts ('a', NULL): 6.
CREATE TABLE hjw_o(id int, g int, f boolean, s text, w int);
INSERT INTO hjw_o VALUES (1, 1, true, 'a', 2), (2, 1, NULL, NULL, 3);
SELECT add_provenance('hjw_o');
SELECT create_provenance_mapping('hjw_o_w', 'hjw_o', 'w');
CREATE TABLE hjw_r AS
  SELECT g, sr_counting(provenance(), 'hjw_o_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_o GROUP BY g HAVING bool_and(f);
SELECT remove_provenance('hjw_r');
SELECT 'bool_and' AS q, g, n, b FROM hjw_r;
DROP TABLE hjw_r;
CREATE TABLE hjw_r AS
  SELECT g, sr_counting(provenance(), 'hjw_o_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_o GROUP BY g HAVING min(s) < 'b';
SELECT remove_provenance('hjw_r');
SELECT 'min over text' AS q, g, n, b FROM hjw_r;
DROP TABLE hjw_r;

-- A group too large for the joint enumeration (25 rows, 7 of them NULL) is
-- evaluated without the rows read for no value, with a warning: the answer
-- is then exact in a Boolean algebra only.
CREATE TABLE hjw_big(id int, v int);
INSERT INTO hjw_big SELECT i, CASE WHEN i <= 7 THEN NULL ELSE 1 END
  FROM generate_series(1, 25) i;
SELECT add_provenance('hjw_big');
CREATE TABLE hjw_r AS
  SELECT sr_counting(provenance(), 'hjw_o_w') AS n, sr_boolean(provenance()) AS b
  FROM hjw_big HAVING min(v) < 2;
SELECT remove_provenance('hjw_r');
SELECT 'too large' AS q, n, b FROM hjw_r;
DROP TABLE hjw_r;
DROP TABLE hjw_big;
DROP TABLE hjw_o_w;
DROP TABLE hjw_o;

CREATE TABLE hjw_dept(d int, name int, w int);
INSERT INTO hjw_dept VALUES (1, 30, 2), (2, 30, 2), (3, 40, 2);
CREATE TABLE hjw_e(id int, dept int, w int);
INSERT INTO hjw_e VALUES (1, 1, 3), (2, 3, 3);
SELECT add_provenance('hjw_dept');
SELECT add_provenance('hjw_e');
SELECT create_provenance_mapping('hjw_dept_w', 'hjw_dept', 'w');
SELECT create_provenance_mapping('hjw_e_w', 'hjw_e', 'w');
CREATE TABLE hjw_w AS
  SELECT * FROM hjw_dept_w UNION ALL SELECT * FROM hjw_e_w;

-- dept 1 (d) has one employee (e): d ⊗ ((1 ⊖ e) ⊕ e) = 2 ⊗ 3 = 6.  dept 2
-- has none: d ⊗ (1 ⊖ 0) = 2.  dept 3 fails name = 30 and has one employee:
-- d ⊗ e = 6.
CREATE TABLE hjw_r AS
  SELECT d, sr_counting(provenance(), 'hjw_w') AS n,
         sr_boolean(provenance()) AS b
  FROM hjw_dept
  WHERE name = 30 OR EXISTS (SELECT 1 FROM hjw_e WHERE hjw_e.dept = hjw_dept.d);
SELECT remove_provenance('hjw_r');
SELECT 'name = 30 OR EXISTS' AS q, d, n, b FROM hjw_r ORDER BY d;
DROP TABLE hjw_r;

DROP TABLE hjw_w;
DROP TABLE hjw_dept_w;
DROP TABLE hjw_e_w;
DROP TABLE hjw_emp_w;
DROP TABLE hjw_e;
DROP TABLE hjw_dept;
DROP TABLE hjw_emp;
