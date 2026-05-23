\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Regression: a UNION mixing a branch with NO provenance source (constant
-- rows, or an untracked relation) with a provenance-tracked branch.  Before the
-- fix this raised "resjunk output columns are not implemented" from the
-- planner, because the untracked branch was left without a provsql column,
-- misaligning the set-operation columns.  Untracked rows are unconditionally
-- present, so their provenance is the multiplicative identity gate_one (𝟙).

-- (1) UNION ALL: untracked constant branch (first) + tracked branch
CREATE TABLE union_untracked1 AS
  SELECT name, sr_formula(provenance(), 'personnel_name') AS formula
  FROM (
    SELECT 'CONSTANT'::varchar AS name
    UNION ALL
    SELECT name FROM personnel WHERE city='Paris'
  ) t;
SELECT remove_provenance('union_untracked1');
SELECT * FROM union_untracked1 ORDER BY name, formula;
DROP TABLE union_untracked1;

-- (2) UNION (distinct): untracked constant branch + tracked branch
CREATE TABLE union_untracked2 AS
  SELECT name, sr_formula(provenance(), 'personnel_name') AS formula
  FROM (
    SELECT 'CONSTANT'::varchar AS name
    UNION
    SELECT name FROM personnel WHERE city='Paris'
  ) t;
SELECT remove_provenance('union_untracked2');
SELECT * FROM union_untracked2 ORDER BY name, formula;
DROP TABLE union_untracked2;

-- (3) untracked RELATION branch (not just a constant) + tracked branch
CREATE TABLE untracked_rel(name varchar);
INSERT INTO untracked_rel VALUES ('Xavier'), ('Yara');
CREATE TABLE union_untracked3 AS
  SELECT name, sr_formula(provenance(), 'personnel_name') AS formula
  FROM (
    SELECT name FROM untracked_rel
    UNION ALL
    SELECT name FROM personnel WHERE city='Paris'
  ) t;
SELECT remove_provenance('union_untracked3');
SELECT * FROM union_untracked3 ORDER BY name, formula;
DROP TABLE union_untracked3;
DROP TABLE untracked_rel;

-- (4) order independence: tracked branch FIRST, untracked branch SECOND
CREATE TABLE union_untracked4 AS
  SELECT name, sr_formula(provenance(), 'personnel_name') AS formula
  FROM (
    SELECT name FROM personnel WHERE city='Paris'
    UNION ALL
    SELECT 'CONSTANT'::varchar AS name
  ) t;
SELECT remove_provenance('union_untracked4');
SELECT * FROM union_untracked4 ORDER BY name, formula;
DROP TABLE union_untracked4;
