CREATE EXTENSION "uuid-ossp";
CREATE EXTENSION provsql;

SET search_path TO public, provsql;

/* STIF dataset */

CREATE TABLE routes (
    route_id text NOT NULL,
    agency_id text,
    route_short_name text,
    route_long_name text,
    route_desc text,
    route_type integer,
    route_url text,
    route_color character(6),
    route_text_color character(6),
    route_sort_order integer
);
CREATE TABLE stops (
    stop_id text NOT NULL,
    stop_code text,
    stop_name text,
    stop_desc text,
    stop_lon double precision,
    stop_lat double precision,
    zone_id integer,
    stop_url text,
    location_type integer,
    parent_station text,
    stop_timezone text,
    level_id text,
    wheelchair_boarding integer,
    platform_code text,
    stop_access text
);
CREATE TABLE trips (
    route_id text,
    service_id text,
    trip_id text NOT NULL,
    trip_headsign text,
    trip_short_name text,
    direction_id integer,
    block_id text,
    shape_id text,
    wheelchair_accessible integer,
    bikes_allowed integer
);
CREATE TABLE stop_times (
    trip_id text NOT NULL,
    arrival_time character(8),
    departure_time character(8),
    start_pickup_drop_off_window text,
    end_pickup_drop_off_window text,
    stop_id text,
    stop_sequence integer NOT NULL,
    pickup_type integer,
    drop_off_type integer,
    local_zone_id text,
    stop_headsign text,
    timepoint integer,
    pickup_booking_rule_id text,
    drop_off_booking_rule_id text
);

-- Files can be downloaded from
-- https://www.data.gouv.fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/
-- Direct zip: https://eu.ftp.opendatasoft.com/stif/GTFS/IDFM-gtfs.zip
\copy routes from 'routes.txt' WITH (FORMAT CSV, HEADER);
\copy stop_times from 'stop_times.txt' WITH (FORMAT CSV, HEADER);
\copy stops from 'stops.txt' WITH (FORMAT CSV, HEADER);
\copy trips from 'trips.txt' WITH (FORMAT CSV, HEADER);

ALTER TABLE ONLY routes
    ADD CONSTRAINT routes_pkey PRIMARY KEY (route_id);
ALTER TABLE ONLY stop_times
    ADD CONSTRAINT stop_times_pkey PRIMARY KEY (trip_id, stop_sequence);
ALTER TABLE ONLY stops
    ADD CONSTRAINT stops_pkey PRIMARY KEY (stop_id);
ALTER TABLE ONLY trips
    ADD CONSTRAINT trips_pkey PRIMARY KEY (trip_id);
CREATE INDEX stop_times_stop_id_idx ON stop_times USING btree (stop_id);
CREATE INDEX stop_times_trip_id_idx ON stop_times USING btree (trip_id);
CREATE INDEX stops_parent_station_idx ON stops USING btree (parent_station);
CREATE INDEX stops_stop_name_idx ON stops USING btree (stop_name);
CREATE INDEX trips_route_id_idx ON trips USING btree (route_id);

-- Normalize GTFS wheelchair values: 0=no info→1 (accessible by default),
-- 1=accessible→1, 2=not accessible→0
UPDATE stops SET wheelchair_boarding = CASE WHEN wheelchair_boarding = 2 THEN 0 ELSE 1 END;
UPDATE trips SET wheelchair_accessible = CASE WHEN wheelchair_accessible = 2 THEN 0 ELSE 1 END;

select add_provenance('trips');
select add_provenance('stops');
\set ECHO none
SET search_path TO public, provsql;

SELECT create_provenance_mapping('w_trips','trips','wheelchair_accessible');
SELECT create_provenance_mapping('w_stops','stops','wheelchair_boarding');
CREATE TABLE wheelchair as select * from w_trips union select * from w_stops;
CREATE INDEX on wheelchair(provenance);
