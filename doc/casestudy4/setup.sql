-- Case Study 4: Government Ministers Over Time (Temporal)
-- Setup script – run from the directory containing the CSV data files:
--   cd doc/casestudy4/data
--   psql -d mydb -f setup.sql

CREATE TABLE person (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    gender TEXT CHECK (gender IN ('male', 'female', 'other')),
    birth DATE,
    death DATE
);

CREATE TABLE holds (
    id INTEGER REFERENCES person(id),
    position TEXT NOT NULL,
    country CHAR(2) NOT NULL,
    start DATE NOT NULL,
    until DATE,
    PRIMARY KEY (id, position, start)
);

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

-- Convert DATE columns to tstzmultirange validity intervals
ALTER TABLE person ADD COLUMN validity tstzmultirange;
ALTER TABLE holds  ADD COLUMN validity tstzmultirange;

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

-- Create temporal validity mappings
SELECT create_provenance_mapping_view('person_validity', 'person', 'validity');
SELECT create_provenance_mapping_view('holds_validity',  'holds',  'validity');

-- Extend the global time_validity_view to include both mappings
ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_update;
CREATE VIEW provsql.time_validity_view AS
    SELECT * FROM provsql.time_validity_view_update
  UNION ALL
    SELECT * FROM person_validity
  UNION ALL
    SELECT * FROM holds_validity;

-- Convenience view joining person and holds
CREATE VIEW person_position AS
SELECT DISTINCT name, position
FROM person JOIN holds ON person.id = holds.id
WHERE country = 'FR';
