\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Mini Case Study 3: wheelchair accessibility over synthetic GTFS data.
-- Tests sr_boolean with integer 0/1 values in a UNION mapping table
-- (regression for the pec_bool integer-to-boolean conversion fix).

CREATE TABLE cs3_routes (
    route_id        text PRIMARY KEY,
    route_long_name text
);
CREATE TABLE cs3_stops (
    stop_id            text PRIMARY KEY,
    stop_name          text,
    location_type      integer,
    parent_station     text,
    wheelchair_boarding integer
);
CREATE TABLE cs3_trips (
    trip_id              text PRIMARY KEY,
    route_id             text,
    wheelchair_accessible integer
);
CREATE TABLE cs3_stop_times (
    trip_id       text,
    stop_id       text,
    stop_sequence integer,
    PRIMARY KEY (trip_id, stop_sequence)
);

INSERT INTO cs3_routes VALUES ('R1', 'Line 1');

-- S0: parent station, GTFS wheelchair_boarding=0 (no info) → normalized to 1
-- S1: accessible child stop of S0
-- S2: accessible destination (wheelchair_boarding=1 → stays 1)
-- S3: inaccessible destination (wheelchair_boarding=2 → normalized to 0)
INSERT INTO cs3_stops VALUES
    ('S0', 'Depart',          1, NULL, 0),
    ('S1', 'Depart Platform', 0, 'S0', 1),
    ('S2', 'DestA',           0, NULL, 1),
    ('S3', 'DestB',           0, NULL, 2);

INSERT INTO cs3_trips VALUES
    ('T1', 'R1', 1),
    ('T2', 'R1', 1);

-- T1: Depart Platform → DestA (fully accessible)
-- T2: Depart Platform → DestB (stop DestB not accessible)
INSERT INTO cs3_stop_times VALUES
    ('T1', 'S1', 1), ('T1', 'S2', 2),
    ('T2', 'S1', 1), ('T2', 'S3', 2);

-- Normalize: GTFS 0=no info→1, 1=accessible→1, 2=not accessible→0
UPDATE cs3_stops SET wheelchair_boarding    = CASE WHEN wheelchair_boarding    = 2 THEN 0 ELSE 1 END;
UPDATE cs3_trips SET wheelchair_accessible  = CASE WHEN wheelchair_accessible  = 2 THEN 0 ELSE 1 END;

SELECT add_provenance('cs3_trips');
SELECT add_provenance('cs3_stops');

SELECT create_provenance_mapping('w_cs3_trips', 'cs3_trips', 'wheelchair_accessible');
SELECT create_provenance_mapping('w_cs3_stops', 'cs3_stops', 'wheelchair_boarding');
CREATE TABLE cs3_wheelchair AS SELECT * FROM w_cs3_trips UNION SELECT * FROM w_cs3_stops;
CREATE INDEX ON cs3_wheelchair(provenance);

-- Boolean accessibility per destination (materialized first, GROUP BY outside ProvSQL)
CREATE TEMP TABLE cs3_bools AS
SELECT s2.stop_name,
       sr_boolean(provenance(), 'cs3_wheelchair') AS accessible
FROM cs3_stops      s0
JOIN cs3_stops      s1  ON s1.parent_station    = s0.stop_id
JOIN cs3_stop_times t1  ON s1.stop_id           = t1.stop_id
JOIN cs3_stop_times t2  ON t1.trip_id           = t2.trip_id
                       AND t1.stop_sequence     < t2.stop_sequence
JOIN cs3_stops      s2  ON s2.stop_id           = t2.stop_id
JOIN cs3_trips      u2  ON u2.trip_id           = t2.trip_id
JOIN cs3_routes     r2  ON r2.route_id          = u2.route_id
WHERE s0.stop_name = 'Depart';

SELECT remove_provenance('cs3_bools');
SELECT stop_name, bool_or(accessible) AS accessible
FROM cs3_bools
GROUP BY stop_name
ORDER BY stop_name;

-- Formula for the inaccessible destination: should show a 0 factor
CREATE TEMP TABLE cs3_formula AS
SELECT s2.stop_name,
       sr_formula(provenance(), 'cs3_wheelchair') AS formula
FROM cs3_stops      s0
JOIN cs3_stops      s1  ON s1.parent_station    = s0.stop_id
JOIN cs3_stop_times t1  ON s1.stop_id           = t1.stop_id
JOIN cs3_stop_times t2  ON t1.trip_id           = t2.trip_id
                       AND t1.stop_sequence     < t2.stop_sequence
JOIN cs3_stops      s2  ON s2.stop_id           = t2.stop_id
JOIN cs3_trips      u2  ON u2.trip_id           = t2.trip_id
JOIN cs3_routes     r2  ON r2.route_id          = u2.route_id
WHERE s0.stop_name = 'Depart'
  AND s2.stop_name = 'DestB';

SELECT remove_provenance('cs3_formula');
SELECT stop_name, formula FROM cs3_formula;

DROP TABLE cs3_bools;
DROP TABLE cs3_formula;
DROP TABLE cs3_wheelchair;
DROP TABLE w_cs3_stops;
DROP TABLE w_cs3_trips;
SELECT remove_provenance('cs3_stops');
SELECT remove_provenance('cs3_trips');
DROP TABLE cs3_stop_times;
DROP TABLE cs3_trips;
DROP TABLE cs3_stops;
DROP TABLE cs3_routes;
