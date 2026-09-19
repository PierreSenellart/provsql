\set ECHO none
\pset format unaligned
SET search_path TO provsql_test, provsql;

-- A part of a query evaluated as plain SQL, not tracked: a warning, naming a
-- relation the rest of the statement also tracks; refused under
-- provsql.implicit_freeze = 'error' when there is one; silent when marked
-- plain().
CREATE TABLE if_a(id int, v int);
CREATE TABLE if_b(id int, w int);
INSERT INTO if_a VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO if_b VALUES (1, 2), (2, 3);
SELECT add_provenance('if_a');
SELECT add_provenance('if_b');

-- Reading another relation (coherent), then the same one (incoherent)
CREATE TABLE if_r AS
  SELECT id, generate_series(1, (SELECT max(w) FROM if_b)) AS g FROM if_a;
DROP TABLE if_r;
CREATE TABLE if_r AS
  SELECT id, generate_series(1, (SELECT min(v) / 10 FROM if_a)) AS g FROM if_a;
DROP TABLE if_r;
CREATE TABLE if_r AS SELECT id, lag(v) OVER (ORDER BY id) AS l FROM if_a;
DROP TABLE if_r;

SET provsql.implicit_freeze = 'error';
-- Coherent: still a warning
CREATE TABLE if_r AS
  SELECT id, generate_series(1, (SELECT max(w) FROM if_b)) AS g FROM if_a;
DROP TABLE if_r;
-- Incoherent: refused
SELECT id, generate_series(1, (SELECT min(v) / 10 FROM if_a)) AS g FROM if_a;
SELECT id, lag(v) OVER (ORDER BY id) AS l FROM if_a;
SELECT v, count(*) FROM if_a GROUP BY v ORDER BY count(*) DESC, v LIMIT 1;
-- Marked plain(): silent, the value of the data as it is
CREATE TABLE if_r AS
  SELECT id, generate_series(1, plain((SELECT min(v) / 10 FROM if_a))) AS g,
         plain(lag(v) OVER (ORDER BY id)) AS l,
         plain((SELECT count(*) FROM if_a c WHERE c.v > a.v)) AS above
  FROM if_a a;
SELECT remove_provenance('if_r');
SELECT id, g, l, above FROM if_r ORDER BY id, g;
DROP TABLE if_r;
CREATE TABLE if_r AS
  SELECT v, count(*) AS n FROM if_a GROUP BY v ORDER BY v LIMIT plain(1);
SELECT remove_provenance('if_r');
SELECT v, n::text AS n FROM if_r;
DROP TABLE if_r;
RESET provsql.implicit_freeze;

-- A table read as plain SQL in FROM: plain(NULL::t), its columns without the
-- provenance one, no provenance of its own (next to a tracked relation, the
-- rows carry that one's only), even with a dropped column.
ALTER TABLE if_b ADD COLUMN gone int;
ALTER TABLE if_b DROP COLUMN gone;
ALTER TABLE if_b ADD COLUMN note text DEFAULT 'n';
SELECT * FROM plain(NULL::if_b) ORDER BY id;
SELECT create_provenance_mapping('if_m', 'if_a', 'id');
CREATE TABLE if_r AS
  SELECT a.id, b.w, b.note, sr_formula(provenance(), 'if_m') AS f
  FROM if_a a JOIN plain(NULL::if_b) b ON b.id = a.id;
SELECT remove_provenance('if_r');
SELECT * FROM if_r ORDER BY id;
DROP TABLE if_r, if_m;
SET provsql.active = off;
SELECT count(*) AS n FROM plain(NULL::if_b);
RESET provsql.active;

-- An aggregate result read as a plain value by a function is evaluated as
-- plain SQL: refused under 'error'; an explicit cast says so, and runs.
SET provsql.implicit_freeze = 'error';
SELECT round(avg(v)) AS r FROM if_a;
CREATE TABLE if_r AS SELECT count(*)::numeric AS n FROM if_a;
RESET provsql.implicit_freeze;
SELECT remove_provenance('if_r');
SELECT * FROM if_r;
DROP TABLE if_r;

SELECT remove_provenance('if_a');
SELECT remove_provenance('if_b');
DROP TABLE if_a, if_b;
