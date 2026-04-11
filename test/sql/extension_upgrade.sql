\set ECHO none
\pset format unaligned

-- This test runs LAST. It drops the current extension (CASCADE drops
-- all provenance-tracked tables created by earlier tests), reinstalls
-- at the oldest supported version using the frozen
-- sql/provsql--1.0.0.sql fixture, and applies the full committed
-- chain of upgrade scripts up to the current default_version.
-- It exercises:
--   * the frozen 1.0.0 install-script fixture
--   * every sql/upgrades/provsql--*--*.sql script in order
--   * the DATA wildcard in Makefile.internal that ships them
--   * binary stability of the mmap format across versions

SET client_min_messages = WARNING;
DROP EXTENSION provsql CASCADE;
RESET client_min_messages;

CREATE EXTENSION provsql VERSION '1.0.0';

SELECT extversion FROM pg_extension WHERE extname = 'provsql';

ALTER EXTENSION provsql UPDATE;

SELECT pe.extversion = ae.default_version AS upgrade_reached_default_version
  FROM pg_extension pe, pg_available_extensions ae
  WHERE pe.extname = 'provsql' AND ae.name = 'provsql';

-- Smoke test: basic provenance tracking still works after the upgrade.
SET search_path TO public, provsql;

CREATE TABLE upgrade_smoke (name text);
INSERT INTO upgrade_smoke VALUES ('alice'), ('bob'), ('carol');
SELECT add_provenance('upgrade_smoke');

SELECT create_provenance_mapping('upgrade_smoke_map', 'upgrade_smoke', 'name');

CREATE TABLE upgrade_result AS
  SELECT name, sr_formula(provenance(), 'upgrade_smoke_map') AS formula
  FROM upgrade_smoke;
SELECT remove_provenance('upgrade_result');
SELECT * FROM upgrade_result ORDER BY name;
