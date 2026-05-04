\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

SET TIME ZONE 'UTC';
SET datestyle = 'iso';

-- Create tables WITHOUT validity initially (mirrors the data.sql pattern:
-- set validity before add_provenance so update_statement_trigger doesn't fire
-- and cause PK conflicts on re-insertion from OLD_TABLE)
CREATE TABLE cs4_person (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    birth   DATE
);

CREATE TABLE cs4_holds (
    id          INTEGER REFERENCES cs4_person(id),
    position    TEXT NOT NULL,
    country     CHAR(2) NOT NULL,
    start_date  DATE NOT NULL,
    end_date    DATE,
    PRIMARY KEY (id, position, start_date)
);

INSERT INTO cs4_person (id, name, birth) VALUES
  (1, 'Alice Blanc',  '1960-01-01'),
  (2, 'Bernard Chai', '1965-01-01'),
  (3, 'Carla Diop',   '1970-01-01');

INSERT INTO cs4_holds (id, position, country, start_date, end_date) VALUES
  (1, 'Prime Minister', 'FR', '2010-01-01', '2016-01-01'),
  (2, 'Prime Minister', 'FR', '2016-01-01', '2022-01-01'),
  (3, 'Prime Minister', 'FR', '2022-01-01', NULL);

-- Add validity columns and set them BEFORE add_provenance
ALTER TABLE cs4_person ADD COLUMN validity tstzmultirange;
ALTER TABLE cs4_holds  ADD COLUMN validity tstzmultirange;

UPDATE cs4_person SET validity = tstzmultirange(tstzrange(birth, NULL));
UPDATE cs4_holds  SET validity = tstzmultirange(tstzrange(start_date, end_date));

ALTER TABLE cs4_holds DROP COLUMN start_date;
ALTER TABLE cs4_holds DROP COLUMN end_date;
ALTER TABLE cs4_person DROP COLUMN birth;

-- Now enable update tracking and add provenance (triggers installed here)
SET provsql.update_provenance = on;
DELETE FROM update_provenance;

SELECT add_provenance('cs4_person');
SELECT add_provenance('cs4_holds');

-- Create validity mapping views
SELECT create_provenance_mapping_view('cs4_person_validity', 'cs4_person', 'validity');
SELECT create_provenance_mapping_view('cs4_holds_validity',  'cs4_holds',  'validity');

-- Extend the time_validity_view to include person and holds validity sources
ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_cs4_save;
CREATE VIEW provsql.time_validity_view AS
    SELECT * FROM provsql.time_validity_view_cs4_save
  UNION ALL SELECT * FROM cs4_person_validity
  UNION ALL SELECT * FROM cs4_holds_validity;

CREATE VIEW cs4_person_position AS
SELECT DISTINCT name, position
FROM cs4_person JOIN cs4_holds ON cs4_person.id = cs4_holds.id
WHERE country = 'FR';

-- Step 3: union_tstzintervals — Bernard Chai's Prime Minister tenure
CREATE TABLE result_cs4_uttz AS
SELECT position, union_tstzintervals(provenance(), 'provsql.time_validity_view') AS valid
FROM cs4_person JOIN cs4_holds ON cs4_person.id = cs4_holds.id
WHERE name = 'Bernard Chai';

SELECT remove_provenance('result_cs4_uttz');
SELECT position, valid FROM result_cs4_uttz ORDER BY valid;
DROP TABLE result_cs4_uttz;

-- Step 4: timeslice — who was Prime Minister during 2015-2020?
CREATE TABLE result_cs4_ts AS
SELECT * FROM timeslice('cs4_person_position', '2015-01-01', '2020-01-01')
  AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid);

SELECT remove_provenance('result_cs4_ts');
SELECT name, validity FROM result_cs4_ts ORDER BY validity;
DROP TABLE result_cs4_ts;

-- Step 5: history — all Prime Ministers
CREATE TABLE result_cs4_hist AS
SELECT * FROM history('cs4_person_position',
                      ARRAY['position'], ARRAY['Prime Minister'])
  AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid);

SELECT remove_provenance('result_cs4_hist');
SELECT name, validity FROM result_cs4_hist ORDER BY validity;
DROP TABLE result_cs4_hist;

-- Step 6: timetravel — government on 2018-06-15
CREATE TABLE result_cs4_tt AS
SELECT * FROM timetravel('cs4_person_position', '2018-06-15')
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid);

SELECT remove_provenance('result_cs4_tt');
SELECT name, position FROM result_cs4_tt ORDER BY name;
DROP TABLE result_cs4_tt;

-- Steps 7-8: data modification — replace Carla Diop with Dana Evans
DELETE FROM cs4_holds
WHERE position = 'Prime Minister'
  AND id = (SELECT id FROM cs4_person WHERE name = 'Carla Diop');

INSERT INTO cs4_person (id, name, validity)
  VALUES (100, 'Dana Evans', NULL);
INSERT INTO cs4_holds (id, position, country, validity)
  VALUES (100, 'Prime Minister', 'FR', tstzmultirange(tstzrange('2022-01-01', NULL)));

-- Set fixed timestamps in update_provenance for deterministic output
UPDATE update_provenance
SET valid_time = CASE
  WHEN query_type = 'DELETE' THEN
    tstzmultirange(tstzrange('2025-01-01 00:00:00+00', NULL))
  WHEN query_type = 'INSERT' THEN
    tstzmultirange(tstzrange('2025-01-01 00:00:01+00', NULL))
  ELSE valid_time
END;

-- Verify modification: only Dana Evans should be PM on 2025-06-01
CREATE TABLE result_cs4_after AS
SELECT * FROM timetravel('cs4_person_position', '2025-06-01')
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid);

SELECT remove_provenance('result_cs4_after');
SELECT name, position FROM result_cs4_after ORDER BY name;
DROP TABLE result_cs4_after;

-- Step 9: undo all modifications
CREATE TABLE undo_result AS
  SELECT undo(provsql) FROM update_provenance ORDER BY ts;
DROP TABLE undo_result;

-- Verify undo: Carla Diop should be restored as PM on 2023-01-01
-- (undo tokens must remain in update_provenance for evaluation)
CREATE TABLE result_cs4_undo AS
SELECT * FROM timetravel('cs4_person_position', '2023-01-01')
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid);

SELECT remove_provenance('result_cs4_undo');
SELECT name, position FROM result_cs4_undo ORDER BY name;
DROP TABLE result_cs4_undo;

-- Clean up (order matters: drop views that depend on tables first)
DROP VIEW cs4_person_position;
DROP VIEW provsql.time_validity_view;
DROP VIEW cs4_holds_validity;
DROP VIEW cs4_person_validity;
ALTER VIEW provsql.time_validity_view_cs4_save RENAME TO time_validity_view;
DROP TABLE cs4_holds;
DROP TABLE cs4_person;
DELETE FROM update_provenance;

SET provsql.update_provenance = off;
