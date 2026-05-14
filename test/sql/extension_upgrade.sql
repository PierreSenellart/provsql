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

-- PostgreSQL < 12 disallows multiple ALTER TYPE ADD VALUE statements
-- inside a single multi-statement script (which is how an extension
-- upgrade runs).  The 1.4.0 -> 1.5.0 upgrade adds three new enum
-- values for the continuous-distribution gate types, so the in-place
-- upgrade test is skipped on those versions; fresh installs are
-- unaffected (the assembled install script is run differently).
-- pg_regress matches against extension_upgrade_1.out on the skip
-- path.
SELECT current_setting('server_version_num')::int >= 120000
  AS pg_supports_enum_upgrade
\gset
\if :pg_supports_enum_upgrade

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

-- 1.5.0 surface check: a closed-form moment evaluation. expected() of
-- a Normal(2, 1) is exactly 2, so this both exercises the new
-- random_variable type / gate_rv / Expectation evaluator and the
-- polymorphic expected dispatcher landed by this upgrade.
SELECT expected(provsql.normal(2, 1)) AS expected_normal_mean;

\else

\echo extension_upgrade: skipped on PostgreSQL < 12 (ALTER TYPE ADD VALUE restriction)

\endif
