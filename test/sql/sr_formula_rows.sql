\set ECHO none
\pset format unaligned

-- sr_formula_rows: the formula of sr_formula, as rows of bounded size.
-- Every leaf is in the mapping, so the formulas print no UUID.
CREATE TABLE fr_r(id int, v int);
INSERT INTO fr_r SELECT i, i FROM generate_series(1, 12) i;
SELECT add_provenance('fr_r');
CREATE TABLE fr_s(id int);
INSERT INTO fr_s SELECT i FROM generate_series(1, 25) i;
SELECT add_provenance('fr_s');
SET provsql.active = off;
CREATE TABLE fr_map AS
  SELECT 'r' || id AS value, provsql AS provenance FROM fr_r
  UNION ALL SELECT 's' || id, provsql FROM fr_s;
SET provsql.active = on;

-- A formula within max_size is one row, the formula of sr_formula.
CREATE TABLE fr_small AS
  SELECT r1.id, provenance() AS p FROM fr_r r1, fr_r r2
  WHERE r1.id = r2.v AND r1.id <= 2;
SELECT remove_provenance('fr_small');
SELECT id, f.name, f.formula, f.formula = sr_formula(p, 'fr_map') AS same
FROM fr_small, sr_formula_rows(p, 'fr_map') f ORDER BY id;

-- ORDER BY ... LIMIT: a row is kept when fewer than 3 rows rank before it.
-- The count of those rows is a sub-formula of more than 10 symbols, named
-- #2 and given its own row.
CREATE TABLE fr_limit AS
  SELECT v, provenance() AS p FROM (SELECT v FROM fr_r ORDER BY v LIMIT 3) x;
SELECT remove_provenance('fr_limit');
SELECT f.name, f.formula
FROM fr_limit, sr_formula_rows(p, 'fr_map', 10) f WHERE v = 12;

-- A comparison on an aggregate.  With 3 contributors, its possible worlds
-- are few: it is expanded, as sr_formula does.  With 9, they are too many
-- for rows of 10 symbols: it is written as the comparison, the sum named.
CREATE TABLE fr_cmp AS
  SELECT g, provenance() AS p FROM (
    SELECT v <= 3 AS g, SUM(v) AS t FROM fr_r GROUP BY v <= 3) x
  WHERE t > 2;
SELECT remove_provenance('fr_cmp');
SELECT f.name, f.formula, f.formula = sr_formula(p, 'fr_map') AS same
FROM fr_cmp, sr_formula_rows(p, 'fr_map') f WHERE g;
SELECT f.name, f.formula
FROM fr_cmp, sr_formula_rows(p, 'fr_map', 10) f WHERE NOT g;

-- A sum of 300 terms, at max_size 5: the first row lists them by name,
-- and each has a row.  The aggregate token is taken as the argument.
CREATE TABLE fr_sum AS SELECT SUM(fr_r.v) AS t FROM fr_r, fr_s;
SELECT remove_provenance('fr_sum');
SELECT count(*) AS nb_rows FROM fr_sum, sr_formula_rows(t, 'fr_map', 5);
-- In the select list, rows are produced as LIMIT asks for them.
SELECT (f).name, (f).formula
FROM (SELECT sr_formula_rows(t, 'fr_map', 5) AS f FROM fr_sum LIMIT 4) x;
SELECT f.name, f.formula
FROM fr_sum, sr_formula_rows(t, 'fr_map', 5) f WHERE f.name = '#1.300';

-- No token, no rows; max_size must be positive.
SELECT count(*) AS null_token FROM sr_formula_rows(NULL, 'fr_map');
SELECT * FROM sr_formula_rows(gate_one(), NULL, 0);

DROP TABLE fr_small, fr_limit, fr_cmp, fr_sum, fr_map, fr_r, fr_s;
