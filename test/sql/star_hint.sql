\set ECHO none
\pset format unaligned

-- The provsql column that * expands to makes PostgreSQL reject, in parse
-- analysis, a set operation whose other arm lacks it and an INSERT ...
-- SELECT * into an untracked table.  ProvSQL cannot correct the statement
-- there; it adds a hint to the error.
CREATE TABLE star_hint_t(id int, name text);
INSERT INTO star_hint_t VALUES (1, 'a'), (2, 'b');
CREATE TABLE star_hint_p(id int, name text);
SELECT add_provenance('star_hint_t');

-- An arm with * beside one without the column
SELECT 0, '-' FROM star_hint_t WHERE id = 1
UNION SELECT * FROM star_hint_t;
SELECT * FROM star_hint_t EXCEPT SELECT * FROM star_hint_p;

-- INSERT ... SELECT * into an untracked table, also from PL/pgSQL
INSERT INTO star_hint_p SELECT * FROM star_hint_t;
DO $$ BEGIN INSERT INTO star_hint_p SELECT t.* FROM star_hint_t t; END $$;

-- The same errors of a statement without *: no hint
SELECT 1, 2 UNION SELECT 1;
INSERT INTO star_hint_p VALUES (1, 'a', 'b');

-- What the hint says to do
CREATE TABLE star_hint_r AS
  SELECT 0 AS id, '-' AS name FROM star_hint_t WHERE id = 1
  UNION SELECT id, name FROM star_hint_t;
SELECT remove_provenance('star_hint_r');
SELECT * FROM star_hint_r ORDER BY id;
DROP TABLE star_hint_r;
INSERT INTO star_hint_p SELECT id, name FROM star_hint_t;
SELECT * FROM star_hint_p ORDER BY id;

DROP TABLE star_hint_t, star_hint_p;
