-- Case Study 4: Government Ministers Over Time (Temporal)
-- Setup script – run from the directory containing the CSV data files:
--   cd doc/casestudy4/data
--   psql -d mydb -f setup.sql

DROP TABLE IF EXISTS person CASCADE;
CREATE TABLE person (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    gender TEXT CHECK (gender IN ('male', 'female', 'other')),
    birth DATE,
    death DATE
);

DROP TABLE IF EXISTS holds CASCADE;
CREATE TABLE holds (
    id INTEGER REFERENCES person(id),
    position TEXT NOT NULL,
    country CHAR(2) NOT NULL,
    start DATE NOT NULL,
    until DATE,
    PRIMARY KEY (id, position, start)
);

DROP TABLE IF EXISTS party CASCADE;
CREATE TABLE party (
    id INTEGER,
    party TEXT NOT NULL,
    PRIMARY KEY (id, party)
);

\copy person FROM 'FR_person.csv' WITH CSV HEADER;
\copy holds  FROM 'FR_position.csv' WITH CSV HEADER;
\copy party  FROM 'FR_party.csv' WITH CSV HEADER;
\copy person FROM 'SG_person.csv' WITH CSV HEADER;
\copy holds  FROM 'SG_position.csv' WITH CSV HEADER;
\copy party  FROM 'SG_party.csv' WITH CSV HEADER;

-- Convert DATE columns to tstzmultirange validity intervals.
-- Use UTC so that Wikidata dates (which are UTC midnight) are stored correctly
-- regardless of the server timezone.
SET timezone TO 'UTC';

ALTER TABLE person ADD COLUMN IF NOT EXISTS validity tstzmultirange;
ALTER TABLE holds  ADD COLUMN IF NOT EXISTS validity tstzmultirange;

UPDATE person SET validity = tstzmultirange(tstzrange(birth, death));
UPDATE holds  SET validity = tstzmultirange(tstzrange(start, until));

ALTER TABLE holds DROP COLUMN start;
ALTER TABLE holds DROP COLUMN until;

-- Enable ProvSQL temporal support
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET provsql.update_provenance = on;

SET search_path TO public, provsql;

SELECT add_provenance('person');
SELECT add_provenance('holds');

-- Create maintained temporal validity mappings (kept current as rows are
-- inserted, and correct after data modification rewrites a row's provsql)
SELECT create_provenance_mapping('person_validity', 'person', 'validity', maintained => true);
SELECT create_provenance_mapping('holds_validity',  'holds',  'validity', maintained => true);

-- Extend the global time_validity_view to include both mappings
ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_update;
CREATE OR REPLACE VIEW provsql.time_validity_view AS
    SELECT * FROM provsql.time_validity_view_update
  UNION ALL
    SELECT * FROM person_validity
  UNION ALL
    SELECT * FROM holds_validity;

-- Convenience view joining person and holds
CREATE OR REPLACE VIEW person_position AS
SELECT DISTINCT name, position
FROM person JOIN holds ON person.id = holds.id
WHERE country = 'FR';
