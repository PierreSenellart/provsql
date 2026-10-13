\set ECHO none
\pset format unaligned

CREATE TYPE classification_level AS ENUM ('unclassified','restricted','confidential','secret','top_secret','unavailable');

CREATE TABLE personnel(
  id SERIAL PRIMARY KEY,
  name varchar,
  position varchar,
  city varchar,
  classification classification_level
);

INSERT INTO personnel (name,position,city,classification) VALUES
  ('John','Director','New York','unclassified'),
  ('Paul','Janitor','New York','restricted'),
  ('Dave','Analyst','Paris','confidential'),
  ('Ellen','Field agent','Berlin','secret'),
  ('Magdalen','Double agent','Paris','top_secret'),
  ('Nancy','HR','Paris','restricted'),
  ('Susan','Analyst','Berlin','secret');

CREATE TABLE nb_gates (x INT);
INSERT INTO nb_gates SELECT get_nb_gates();

SELECT add_provenance('personnel');

-- Input gates are created lazily: add_provenance creates no gates
SELECT get_nb_gates()-x AS nb_after_add_provenance FROM nb_gates;

-- Force materialization: DISTINCT collapses all 7 rows into one plus gate,
-- creating 7 input gates + 1 plus gate = 8 gates total
DO $$ BEGIN PERFORM DISTINCT 1 FROM personnel; END $$;
SELECT get_nb_gates()-x AS nb_after_select FROM nb_gates;

DROP TABLE nb_gates;

SELECT attname
FROM pg_attribute
WHERE attrelid ='personnel'::regclass AND attnum>1
ORDER BY attname;

SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');

-- Idempotence: re-running both is a NOTICE-and-no-op (notebook cells
-- and setup scripts re-run freely), never an error.
SELECT add_provenance('personnel');
SELECT create_provenance_mapping('personnel_name', 'personnel', 'name');
SELECT count(*) FROM personnel_name;

-- Symmetrically, remove_provenance on an untracked table is a NOTICE-and-no-op
-- rather than an error.
CREATE TABLE untracked (x int);
SELECT remove_provenance('untracked');
DROP TABLE untracked;

-- A view defined before add_provenance keeps the column names the table had
-- then, without its provsql column: a query over it reads the table as it is
-- now, and is tracked all the same, its rows, a count over them and a join
-- with the table itself.
CREATE TABLE stale_src (x int);
INSERT INTO stale_src VALUES (1), (2);
CREATE VIEW stale_v AS SELECT * FROM stale_src;
CREATE VIEW stale_v2 AS SELECT count(*) AS n FROM stale_src;
SELECT add_provenance('stale_src');
SELECT create_provenance_mapping('stale_map', 'stale_src', 'x');
CREATE TABLE stale_r AS
  SELECT x, sr_formula(provenance(), 'stale_map') AS f FROM stale_v;
SELECT remove_provenance('stale_r');
SELECT * FROM stale_r ORDER BY x;
DROP TABLE stale_r;
CREATE TABLE stale_r AS SELECT n, sr_formula(provenance(), 'stale_map') AS f
  FROM stale_v2;
SELECT remove_provenance('stale_r');
SELECT * FROM stale_r;
DROP TABLE stale_r;
CREATE TABLE stale_r AS
  SELECT x, sr_formula(provenance(), 'stale_map') AS f
  FROM stale_v JOIN stale_src USING (x);
SELECT remove_provenance('stale_r');
SELECT * FROM stale_r ORDER BY x;
DROP TABLE stale_r;
DROP VIEW stale_v2, stale_v;
DROP TABLE stale_src, stale_map;
