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

SELECT remove_provenance('if_a');
SELECT remove_provenance('if_b');
DROP TABLE if_a, if_b;
