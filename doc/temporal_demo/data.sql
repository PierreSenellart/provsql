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

UPDATE person SET validity=tstzmultirange(tstzrange(birth,death));
UPDATE holds SET validity=tstzmultirange(tstzrange(start,until));

ALTER TABLE holds DROP COLUMN start;
ALTER TABLE holds DROP COLUMN until;

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

CREATE VIEW person_position AS
SELECT DISTINCT name, position FROM person JOIN holds ON person.id=holds.id
AND country='FR';

--- EXAMPLE QUERIES ---

-- What were the positions of François Bayrou over time?
SELECT position, union_tstzintervals(provenance(),'time_validity_view') valid
FROM person JOIN holds ON person.id=holds.id
WHERE name='François Bayrou' order by valid;

-- What were the ministers during Emmanuel Macron's presidential terms?
SELECT name, validity FROM
  timeslice('person_position', '2017-05-16', NOW())
  AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
  ORDER BY validity;

-- Who were the Ministers of Justice over time?
SELECT name, validity FROM
  history('person_position', ARRAY['position'], ARRAY['Minister of Justice'])
  AS (name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
  ORDER BY validity;

-- What was the government like on 19 June 1981?
SELECT name, position FROM timetravel('person_position', '1981-06-19')
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
ORDER BY position;

-- Fire François Bayrou and replace him with Pierre Senellart
DELETE FROM holds WHERE position='Prime Minister of France' AND id IN
  (SELECT id FROM person WHERE name='François Bayrou');
INSERT INTO person(id, name, gender) VALUES(100000, 'Pierre Senellart', 'male');
INSERT INTO holds(id, position, country) VALUES(100000, 'Prime Minister of France', 'FR');

-- What were the positions of François Bayrou over time, now he has been
-- fired?
SELECT position, union_tstzintervals(provenance(),'time_validity_view') valid
FROM person JOIN holds ON person.id=holds.id
WHERE name='François Bayrou' order by valid;

-- What is the current government composition?
SELECT name, position FROM timetravel('person_position', NOW())
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
ORDER BY position;

-- Undo the changes: Pierre Senellart is out, François Bayrou is in
SELECT undo(provenance()) FROM query_provenance;

-- What were the positions of François Bayrou over time, now he has been
-- fired and then reinstated?
SELECT position, union_tstzintervals(provenance(),'time_validity_view') valid
FROM person JOIN holds ON person.id=holds.id
WHERE name='François Bayrou' order by valid;

-- What is the current government composition?
SELECT name, position FROM timetravel('person_position', NOW())
  AS tt(name TEXT, position TEXT, validity tstzmultirange, provsql uuid)
ORDER BY position;
