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
-- installs gate_assumed + provenance_guard + the
-- per-table metadata seed for the freshly add_provenance'd
-- tables.  The probe asserts the per-row root is the safe-query
-- marker, which is only the case if every link in the chain
-- (enum value, assume_boolean function, set_table_info seed,
-- detector tightening) is in place.
CREATE TABLE upgrade_smoke_right (name text);
INSERT INTO upgrade_smoke_right VALUES ('alice'), ('bob'), ('carol');
SELECT add_provenance('upgrade_smoke_right');
SET provsql.provenance = 'boolean';
CREATE TABLE upgrade_safe_q AS
  SELECT l.name AS name, provenance() AS prov
    FROM upgrade_smoke l, upgrade_smoke_right r
   WHERE l.name = r.name
   GROUP BY l.name;
SELECT remove_provenance('upgrade_safe_q');
SELECT name, get_gate_type(prov) AS root_type
  FROM upgrade_safe_q ORDER BY name;
SET provsql.provenance = 'semiring';

-- 1.6.0 surface check: base-ancestor registry round-trip.  The
-- upgrade does NOT auto-seed metadata for pre-existing tracked
-- relations (they stay at OPAQUE-by-omission, which is the
-- conservative outcome; users opt in explicitly via
-- @c set_table_info / @c set_ancestors).  Here we just exercise
-- the new SQL surface on upgrade_smoke to confirm the registry
-- is callable post-upgrade.
SELECT set_table_info('upgrade_smoke'::regclass::oid, 'tid');
SELECT set_ancestors('upgrade_smoke'::regclass::oid,
                     ARRAY['upgrade_smoke'::regclass::oid]);
SELECT (get_table_info('upgrade_smoke'::regclass::oid)).kind
         AS post_upgrade_kind,
       get_ancestors('upgrade_smoke'::regclass::oid)
       = ARRAY['upgrade_smoke'::regclass::oid]
         AS post_upgrade_ancestry_roundtrip;

-- 1.7.0 surface check: the knowledge-compilation introspection surface
-- and the external-tool probe.  A SELECT DISTINCT collapses two tracked
-- rows into one plus over two input gates; tseytin_cnf_mapping (backed
-- by the new tseytin_cnf_mapping_json C binding) returns one row per
-- input, so the count is the number of provenance inputs (2).  This
-- exercises the new Tseytin-CNF surface in process, with no external
-- compiler.  tool_available('...') on a name that never resolves is the
-- env-independent probe for the new resolver binding.
CREATE TABLE upgrade_kc AS
  SELECT provenance() AS prov
  FROM (SELECT DISTINCT 1 AS one FROM upgrade_smoke WHERE name IN ('alice','bob')) t;
SELECT remove_provenance('upgrade_kc');
SELECT count(*) AS tseytin_input_vars
  FROM upgrade_kc, LATERAL tseytin_cnf_mapping(prov);
SELECT tool_available('provsql-no-such-tool') AS bogus_tool_available;

-- 1.8.0 surface check: the inversion-free annotation wrapper and the
-- external-tool registry, the two headline additions.  annotate() creates
-- a gate of the new 'annotation' enum value; round-tripping it through
-- get_gate_type confirms the upgrade reset the constants cache so the
-- freshly-added value resolves (otherwise create_gate raises "Invalid gate
-- type").  count(*) on the tools view confirms the compiled tool seed is
-- queryable post-upgrade (the seed is env-independent, so the > 0 probe is
-- deterministic regardless of which compilers happen to be installed).
SELECT get_gate_type(annotate(gate_one(), 'cert')) AS annotation_gate_type;
SELECT count(*) > 0 AS tool_registry_seeded FROM tools;

-- 1.9.0 surface check: empty-group-faithful scalar aggregation.  A scalar (no
-- GROUP BY) aggregate now calls the 5-argument provenance_aggregate (the
-- scalar-aggregation flag), so this query would fail outright ("function
-- provenance_aggregate(...) does not exist") if the upgrade left the old 4-arg
-- function.  count(col) skips NULL inputs (the NULL-skipping provenance_semimod)
-- but its empty group is the real value 0, so a scalar HAVING count(col)=0
-- includes the all-absent world.  Over {10, NULL} @ p=0.5: count(x)=0 holds
-- exactly when the non-NULL row 10 is absent (the NULL row is free, present or
-- not), so the probability is P(10 absent)=0.5 -- NOT 0.25 (which is what
-- dropping the empty world would give).  Exercises the 5-arg provenance_aggregate,
-- the NULL-skip semimod, and the count(col) value-aware HAVING enumeration.
CREATE TABLE upgrade_cnt (x int);
INSERT INTO upgrade_cnt VALUES (10), (NULL);
SELECT add_provenance('upgrade_cnt');
DO $$ BEGIN PERFORM set_prob(provenance(), 0.5) FROM upgrade_cnt; END $$;
CREATE TABLE upgrade_cnt_r AS
  SELECT round(probability_evaluate(provenance())::numeric, 4) AS p
  FROM upgrade_cnt HAVING count(x) = 0;
SELECT remove_provenance('upgrade_cnt_r');
SELECT p AS scalar_count_col_empty_world FROM upgrade_cnt_r;

SET client_min_messages = WARNING;
DROP TABLE upgrade_result, upgrade_smoke_map, upgrade_smoke,
           upgrade_smoke_right, upgrade_safe_q, upgrade_kc,
           upgrade_cnt, upgrade_cnt_r;
RESET client_min_messages;

\else

\echo extension_upgrade: skipped on PostgreSQL < 12 (ALTER TYPE ADD VALUE restriction)

\endif
