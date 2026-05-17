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

-- 1.6.0 surface check: a hierarchical CQ rewrite under
-- provsql.boolean_provenance over two tracked atoms (the rewriter
-- bails on single-atom shapes for lack of a root variable, so the
-- smoke needs two).  Exercises the upgrade-script chain that
-- installs gate_assumed_boolean + provenance_guard + the
-- per-table metadata seed for the freshly add_provenance'd
-- tables.  The probe asserts the per-row root is the safe-query
-- marker, which is only the case if every link in the chain
-- (enum value, assume_boolean function, set_table_info seed,
-- detector tightening) is in place.
CREATE TABLE upgrade_smoke_right (name text);
INSERT INTO upgrade_smoke_right VALUES ('alice'), ('bob'), ('carol');
SELECT add_provenance('upgrade_smoke_right');
SET provsql.boolean_provenance = on;
CREATE TABLE upgrade_safe_q AS
  SELECT l.name AS name, provenance() AS prov
    FROM upgrade_smoke l, upgrade_smoke_right r
   WHERE l.name = r.name
   GROUP BY l.name;
SELECT remove_provenance('upgrade_safe_q');
SELECT name, get_gate_type(prov) AS root_type
  FROM upgrade_safe_q ORDER BY name;
SET provsql.boolean_provenance = off;

-- 1.6.0 surface check: base-ancestor registry.  The per-existing-
-- tracked-table backfill in the 1.5.0 -> 1.6.0 upgrade seeds every
-- tracked relation with @c {self}, so a relation freshly tracked
-- after the upgrade chain ran must round-trip @c get_ancestors.
SELECT get_ancestors('upgrade_smoke'::regclass::oid)
       = ARRAY['upgrade_smoke'::regclass::oid] AS ancestry_seeded_to_self;

SET client_min_messages = WARNING;
DROP TABLE upgrade_result, upgrade_smoke_map, upgrade_smoke,
           upgrade_smoke_right, upgrade_safe_q;
RESET client_min_messages;

\else

\echo extension_upgrade: skipped on PostgreSQL < 12 (ALTER TYPE ADD VALUE restriction)

\endif
