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
-- A truncation of an aggregation by keys that read no aggregate: the cut is
-- taken on the data as it is, which is deliberate, and refused here like the
-- rest.  (A truncation whose keys are certain and whose query keeps its rows
-- is the filter of a rank instead, and tracked -- see limit_rank.)
SELECT v, count(*) AS n FROM if_a GROUP BY v ORDER BY v LIMIT 1;
-- Whereas the top-k of an aggregation, tie-breaker and all, is the filter of a
-- rank: tracked, so it passes under 'error', and it answers with every group
-- that is the first in some world.
CREATE TABLE if_r AS
  SELECT v, count(*) AS n FROM if_a GROUP BY v ORDER BY count(*) DESC, v LIMIT 1;
SELECT remove_provenance('if_r');
SELECT v, n::text AS n FROM if_r ORDER BY v;
DROP TABLE if_r;
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
-- The whole row of such a source is its columns, without the place the
-- provenance column holds in the relation: that place is kept in the entry so
-- the attribute numbers still match, and it would otherwise show as a
-- trailing, always empty field.
SELECT b FROM plain(NULL::if_b) b ORDER BY 1;
RESET provsql.active;

-- An aggregate result read as a plain value by a function is evaluated as
-- plain SQL: refused under 'error'; an explicit cast says so, and runs.
-- trunc is such a function; round, floor, ceil, abs, ln, exp and sqrt are not,
-- ProvSQL carrying them as gate operations (see agg_arithmetic), so they stay
-- tracked and pass under 'error'.
SET provsql.implicit_freeze = 'error';
SELECT trunc(avg(v)) AS r FROM if_a;
SELECT round(avg(v)) AS tracked FROM if_a;
CREATE TABLE if_r AS SELECT count(*)::numeric AS n FROM if_a;
RESET provsql.implicit_freeze;
SELECT remove_provenance('if_r');
SELECT * FROM if_r;
DROP TABLE if_r;

-- What plain() is for: the report says "mark it plain() to say so", so marking
-- it has to stop the report.  It did not -- the marker was consumed into the
-- same accessor an unmarked read produces, so a read the user had asked for was
-- reported back to them -- and it now reads through the silent accessor
-- instead.  The value is the one of the database as it is either way.
SELECT plain(sum(v)) AS asked FROM if_a;
-- An unmarked read of the same value still reports, which is the half that
-- must not be lost in making the other silent.
SELECT trunc(avg(v)) AS unmarked FROM if_a;
-- And under implicit_freeze = 'error', a marked read is still allowed while an
-- unmarked one raises: the marker is consent, not a way to silence the policy.
SET provsql.implicit_freeze = 'error';
SELECT plain(sum(v)) AS asked_under_error FROM if_a;
SELECT trunc(avg(v)) AS unmarked_under_error FROM if_a;
RESET provsql.implicit_freeze;

SELECT remove_provenance('if_a');
SELECT remove_provenance('if_b');
DROP TABLE if_a, if_b;
