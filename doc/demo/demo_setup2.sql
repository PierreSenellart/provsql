CREATE EXTENSION "uuid-ossp";
CREATE EXTENSION provsql;

SET search_path TO public, provsql;

/* The provenance formula m-semiring */
CREATE TYPE formula_state AS (
  formula text,
  nbargs int
);

CREATE FUNCTION formula_plus_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊕ ',value),state.nbargs+1);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION formula_times_state(state formula_state, value text)
  RETURNS formula_state AS
$$
BEGIN    
  IF state IS NULL OR state.nbargs=0 THEN
    RETURN (value,1);
  ELSE
    RETURN (concat(state.formula,' ⊗ ',value),state.nbargs+1);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION formula_state2formula(state formula_state)
  RETURNS text AS
$$
  SELECT
    CASE
      WHEN state.nbargs<2 THEN state.formula
      ELSE concat('(',state.formula,')')
    END;
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE AGGREGATE formula_plus(text)
(
  sfunc = formula_plus_state,
  stype = formula_state,
  initcond = '(𝟘,0)',
  finalfunc = formula_state2formula
);

CREATE AGGREGATE formula_times(text)
(
  sfunc = formula_times_state,
  stype = formula_state,
  initcond = '(𝟙,0)',
  finalfunc = formula_state2formula
);

CREATE FUNCTION formula_monus(formula1 text, formula2 text) RETURNS text AS
$$
  SELECT concat('(',formula1,' ⊖ ',formula2,')')
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION formula(token provenance_token, token2value regclass)
  RETURNS text AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    '𝟙'::text,
    'formula_plus',
    'formula_times',
    'formula_monus');
END
$$ LANGUAGE plpgsql;

/* The counting m-semiring */

CREATE FUNCTION counting_plus_state(state INTEGER, value INTEGER)
  RETURNS INTEGER AS
$$
  SELECT CASE WHEN state IS NULL THEN value ELSE state + value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE FUNCTION counting_times_state(state INTEGER, value INTEGER)
  RETURNS INTEGER AS
$$
SELECT CASE WHEN state IS NULL THEN value ELSE state * value END
$$ LANGUAGE SQL IMMUTABLE;

CREATE AGGREGATE counting_plus(INTEGER)
(
  sfunc = counting_plus_state,
  stype = INTEGER,
  initcond = 0
);

CREATE AGGREGATE counting_times(INTEGER)
(
  sfunc = counting_times_state,
  stype = INTEGER,
  initcond = 1
);

CREATE FUNCTION counting_monus(counting1 INTEGER, counting2 INTEGER) RETURNS INTEGER AS
$$
  SELECT CASE WHEN counting1 < counting2 THEN 0 ELSE counting1 - counting2 END
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION counting(token provenance_token, token2value regclass)
  RETURNS INTEGER AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    1,
    'counting_plus',
    'counting_times',
    'counting_monus');
END
$$ LANGUAGE plpgsql;

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
    trip_id character(17) NOT NULL,
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
    trip_id character(17) NOT NULL,
    arrival_time character(8),
    departure_time character(8),
    stop_id character varying(30),
    stop_sequence integer NOT NULL,
    stop_headsign text,
    pickup_type integer,
    drop_off_type integer
);

-- Files can be downloaded from
-- https://opendata.stif.info/explore/dataset/offre-horaires-tc-gtfs-idf/table/
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

/* The BOOLEAN m-semiring */
CREATE FUNCTION boolean_plus_state(state BOOLEAN, value BOOLEAN)
  RETURNS BOOLEAN AS
$$
BEGIN
  IF state IS NULL THEN
    RETURN value;
  ELSE
    RETURN state OR value;
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE FUNCTION boolean_times_state(state BOOLEAN, value BOOLEAN)
  RETURNS BOOLEAN AS
$$
BEGIN    
  IF state IS NULL THEN
    RETURN value;
  ELSE
    RETURN state AND value;
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE AGGREGATE boolean_plus(BOOLEAN)
(
  sfunc = boolean_plus_state,
  stype = BOOLEAN,
  initcond = FALSE
);

CREATE AGGREGATE boolean_times(BOOLEAN)
(
  sfunc = boolean_times_state,
  stype = BOOLEAN,
  initcond = TRUE
);

CREATE FUNCTION boolean_monus(b1 BOOLEAN, b2 BOOLEAN) RETURNS BOOLEAN AS
$$
  SELECT b1 AND NOT b2
$$ LANGUAGE SQL IMMUTABLE STRICT;

CREATE FUNCTION boolean_sr(token provenance_token, token2value regclass)
  RETURNS BOOLEAN AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    TRUE,
    'boolean_plus',
    'boolean_times',
    'boolean_monus');
END
$$ LANGUAGE plpgsql;

SELECT create_provenance_mapping('w_trips','trips','wheelchair_accessible');
SELECT create_provenance_mapping('w_stops','stops','wheelchair_boarding');
CREATE TABLE wheelchair as select * from w_trips union select * from w_stops;
CREATE INDEX on wheelchair(provenance);
