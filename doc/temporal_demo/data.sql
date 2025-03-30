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
    PRIMARY KEY(id,position,start)
);

CREATE TABLE party (
    id INTEGER,
    party TEXT NOT NULL,
    PRIMARY KEY(id,party)
);

\copy person FROM 'FR_person.csv' WITH CSV HEADER;
\copy holds FROM 'FR_position.csv' WITH CSV HEADER;
\copy party FROM 'FR_party.csv' WITH CSV HEADER;
\copy person FROM 'SG_person.csv' WITH CSV HEADER;
\copy holds FROM 'SG_position.csv' WITH CSV HEADER;
\copy party FROM 'SG_party.csv' WITH CSV HEADER;

ALTER TABLE person ADD COLUMN validity tstzmultirange;
ALTER TABLE holds ADD COLUMN validity tstzmultirange;
G
UPDATE person SET validity=tstzmultirange(tstzrange(birth,death));
UPDATE holds SET validity=tstzmultirange(tstzrange(start,until));

CREATE EXTENSION provsql CASCADE;

SELECT provsql.add_provenance('person');
SELECT provsql.add_provenance('holds');

SELECT provsql.create_provenance_mapping_view('person_validity','person','validity');
SELECT provsql.create_provenance_mapping_view('holds_validity','holds','validity');

ALTER VIEW provsql.time_validity_view RENAME TO time_validity_view_update;
CREATE VIEW provsql.time_validity_view AS
  SELECT * FROM provsql.time_validity_view_update
UNION ALL
  SELECT * FROM person_validity
UNION ALL
  SELECT * FROM holds_validity;

SET SEARCH_PATH TO public, provsql;

-- Example queries
SELECT position, union_tstzintervals(provenance(),'time_validity_view') valid
FROM person JOIN holds ON person.id=holds.id
WHERE name='François Bayrou' order by valid;

CREATE VIEW person_position AS
SELECT DISTINCT name, position FROM person JOIN holds ON person.id=holds.id
WHERE holds.start>'1945-01-01'
AND country='FR';

SELECT name, validity FROM
  history('person_position', ARRAY['position'], ARRAY['Minister of Justice'])
  AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
  ORDER BY validity;

SELECT name, position FROM timetravel('person_position', '1981-06-19')
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
ORDER BY position;
