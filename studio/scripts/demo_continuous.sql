-- City air-quality sensor-network fixture for ProvSQL Studio's
-- continuous-distribution demo.  Backs the worked example in
-- doc/source/user/casestudy6.rst (*The City Air-Quality Sensor
-- Network*).
--
-- Exercises every code path the continuous-RV feature touches:
--
--   * gate_rv leaves with all four distribution kinds
--     (normal / uniform / exponential / erlang);
--   * a deterministic reference station that uses the implicit
--     numeric -> random_variable cast (gate_value);
--   * gate_arith (calibration-scaled reading: pm25 * 1.2);
--   * gate_mixture for calibration uncertainty
--     (mixture(p_cal, pm25, pm25 * 1.2));
--   * gate_cmp via WHERE pm25 > 35 (planner-hook rewrite);
--   * UNION ALL across today's batch and the historical batch;
--   * sum / avg over random_variable (semimodule lowering through
--     rv_aggregate_semimod);
--   * HAVING expected(avg(pm25)) > 20 (gate_agg + gate_cmp).
--
-- Run via:
--     psql -d air_quality_demo -f studio/scripts/demo_continuous.sql
-- or, for a one-shot "drop, recreate, load, launch Studio" loader:
--     python3 studio/scripts/demo_continuous.py

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

SET search_path TO public, provsql;

DROP TABLE IF EXISTS readings CASCADE;
DROP TABLE IF EXISTS historical_readings CASCADE;
DROP TABLE IF EXISTS stations CASCADE;
DROP TABLE IF EXISTS calibration_status CASCADE;
DROP TABLE IF EXISTS categories CASCADE;

-- Four monitoring stations across two districts.
CREATE TABLE stations (
  id        TEXT PRIMARY KEY,
  name      TEXT NOT NULL,
  district  TEXT NOT NULL
);

INSERT INTO stations VALUES
  ('s1', 'City Centre',         'centre'),
  ('s2', 'Riverside Park',      'centre'),
  ('s3', 'Industrial Estate',   'east'),
  ('s4', 'Suburban Reference',  'east');

SELECT add_provenance('stations');

-- Per-station calibration probability (Bernoulli).  The high-end and
-- multi-pass units are well calibrated; the low-cost and drift-prone
-- ones less so; the reference station is always in-spec.
CREATE TABLE calibration_status (
  station_id  TEXT PRIMARY KEY REFERENCES stations(id),
  p           DOUBLE PRECISION NOT NULL
);

INSERT INTO calibration_status VALUES
  ('s1', 0.95),
  ('s2', 0.70),
  ('s3', 0.60),
  ('s4', 1.00);

SELECT add_provenance('calibration_status');

-- Regulatory categories (deterministic, no provenance).
CREATE TABLE categories (
  name  TEXT PRIMARY KEY,
  lo    DOUBLE PRECISION NOT NULL,
  hi    DOUBLE PRECISION NOT NULL
);

INSERT INTO categories VALUES
  ('Good',       0.0,    12.0),
  ('Moderate',   12.1,   35.0),
  ('Unhealthy',  35.1,   1000.0);

-- Today's readings.  Five rows of pm25, one per (station, sample),
-- exercising every distribution family plus the implicit numeric cast.
CREATE TABLE readings (
  id          INTEGER PRIMARY KEY,
  station_id  TEXT NOT NULL REFERENCES stations(id),
  ts          TIMESTAMP NOT NULL,
  pm25        provsql.random_variable NOT NULL
);

INSERT INTO readings (id, station_id, ts, pm25) VALUES
  (1, 's1', '2026-05-12 08:00', provsql.normal(28.0, 2.0)),       -- high-end Gaussian
  (2, 's2', '2026-05-12 08:00', provsql.uniform(10.0, 22.0)),     -- low-cost uniform window
  (3, 's3', '2026-05-12 08:00', provsql.exponential(0.04)),       -- drift-prone, mean = 25
  (4, 's4', '2026-05-12 08:00', 15.0),                            -- reference (implicit cast)
  (5, 's1', '2026-05-12 09:00', provsql.normal(40.0, 4.0)),       -- high-end, into Unhealthy
  (6, 's2', '2026-05-12 09:00', provsql.uniform(12.0, 24.0)),     -- low-cost, into Moderate
  (7, 's3', '2026-05-12 09:00', provsql.erlang(3, 0.1)),          -- multi-pass Erlang, mean = 30
  (8, 's4', '2026-05-12 09:00', 16.5);                            -- reference

SELECT add_provenance('readings');

-- Yesterday's batch, used for the UNION ALL step.  Same shape, slightly
-- different distributions (a heatwave bumped the means).
CREATE TABLE historical_readings (
  id          INTEGER PRIMARY KEY,
  station_id  TEXT NOT NULL REFERENCES stations(id),
  ts          TIMESTAMP NOT NULL,
  pm25        provsql.random_variable NOT NULL
);

INSERT INTO historical_readings (id, station_id, ts, pm25) VALUES
  (1, 's1', '2026-05-11 08:00', provsql.normal(34.0, 2.5)),
  (2, 's2', '2026-05-11 08:00', provsql.uniform(15.0, 28.0)),
  (3, 's3', '2026-05-11 08:00', provsql.exponential(0.03)),
  (4, 's4', '2026-05-11 08:00', 18.0),
  (5, 's1', '2026-05-11 09:00', provsql.normal(42.0, 3.0)),
  (6, 's2', '2026-05-11 09:00', provsql.uniform(20.0, 35.0)),
  (7, 's3', '2026-05-11 09:00', provsql.erlang(3, 0.08)),
  (8, 's4', '2026-05-11 09:00', 19.5);

SELECT add_provenance('historical_readings');

-- A provenance mapping so the Studio eval-strip's :sqlfunc:`sr_formula`
-- and PROV-XML export can label leaves with station names rather than
-- raw UUIDs.
DROP TABLE IF EXISTS station_mapping;
CREATE TABLE station_mapping AS
  SELECT s.name AS value, r.provsql AS provenance
  FROM readings r JOIN stations s ON s.id = r.station_id;
SELECT remove_provenance('station_mapping');
CREATE INDEX ON station_mapping(provenance);

\echo ''
\echo 'Loaded the air-quality sensor fixture.  Open Studio against this'
\echo 'database, then walk through the case study:'
\echo ''
\echo '  doc/source/user/casestudy6.rst -- The City Air-Quality Sensor Network'
\echo ''
\echo 'Starter queries (paste into the Studio query box):'
\echo ''
\echo '  -- Step 1: inspect a noisy reading'
\echo '  SELECT id, ts, pm25, provsql FROM readings'
\echo '   WHERE station_id = ''s1'' ORDER BY ts;'
\echo ''
\echo '  -- Step 2: probabilistic threshold (Unhealthy)'
\echo '  SELECT id, station_id, ts, provsql FROM readings'
\echo '   WHERE pm25 > 35;'
\echo ''
\echo '  -- Step 3: per-district aggregates'
\echo '  SELECT s.district, avg(r.pm25) AS avg_pm25, sum(r.pm25) AS total_pm25, provsql'
\echo '    FROM readings r JOIN stations s ON s.id = r.station_id'
\echo '   GROUP BY s.district;'
\echo ''
