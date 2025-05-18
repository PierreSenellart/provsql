CREATE EXTENSION "uuid-ossp";
CREATE EXTENSION provsql;

SET search_path TO public, provsql;

/* STIF dataset */

CREATE TABLE routes (
    route_id character varying(20) NOT NULL,
    agency_id integer,
    route_short_name text,
    route_long_name text,
    route_desc text,
    route_type integer,
    route_url text,
    route_color character(6),
    route_text_color character(6)
);
CREATE TABLE stops (
    stop_id character varying(30) NOT NULL,
    stop_name text,
    stop_desc text,
    stop_lat double precision,
    stop_lon double precision,
    zone_id integer,
    stop_url text,
    location_type integer,
    parent_station character varying(30),
    wheelchair_boarding integer
);
CREATE TABLE trips (
    route_id character varying(20),
    service_id character(5),
    trip_id character(18) NOT NULL,
    trip_headsign text,
    trip_shortname text,
    direction_id integer,
    block_id integer,
    wheelchair_accessible integer,
    bikes_allowed integer,
    trip_desc integer,
    shape_id integer
);
CREATE TABLE stop_times (
    trip_id character(18) NOT NULL,
    arrival_time character(8),
    departure_time character(8),
    stop_id character varying(30),
    stop_sequence integer NOT NULL,
    stop_headsign text,
    pickup_type integer,
    drop_off_type integer
);

-- Files can be downloaded from
-- https://www.data.gouv.fr/fr/datasets/horaires-prevus-sur-les-lignes-de-transport-en-commun-dile-de-france-gtfs-datahub/
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

select add_provenance('trips');
select add_provenance('stops');
select add_provenance('routes');
select add_provenance('stop_times');
\set ECHO none
SET search_path TO public, provsql;

SELECT create_provenance_mapping('w_trips','trips','wheelchair_accessible');
SELECT create_provenance_mapping('w_stops','stops','wheelchair_boarding');
CREATE TABLE wheelchair as select * from w_trips union select * from w_stops;
CREATE INDEX on wheelchair(provenance);
