-- Small temporal-flavoured fixture for ProvSQL Studio's Temporal compiled
-- semiring (sr_temporal). Requires PostgreSQL 14+ (tstzmultirange).
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_temporal.sql
-- Then point the studio at <db> and exercise the suggested queries below
-- by copy-pasting them into the querybox in Circuit mode.
--
-- Seeds two tables:
--   * employments            : 5 rows, provenance-tracked. Each row has a
--                              `validity` tstzmultirange recording the
--                              span over which the (person, company)
--                              employment was active.
--   * employments_validity   : (value tstzmultirange, provenance UUID)
--                              mapping discovered by the Eval strip's
--                              /api/provenance_mappings endpoint.

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

DO $$
BEGIN
  IF current_setting('server_version_num')::int < 140000 THEN
    RAISE EXCEPTION
      'sr_temporal requires PostgreSQL 14+ (tstzmultirange is unavailable).';
  END IF;
END $$;

SET TIME ZONE 'UTC';
SET datestyle = 'iso';
SET search_path TO public, provsql;

DROP TABLE IF EXISTS employments_validity;
DROP TABLE IF EXISTS employments CASCADE;

CREATE TABLE employments (
  id       INTEGER PRIMARY KEY,
  person   TEXT NOT NULL,
  company  TEXT NOT NULL,
  validity tstzmultirange NOT NULL
);

INSERT INTO employments (id, person, company, validity) VALUES
  (1, 'Alice', 'Acme',     '{[2020-01-01, 2022-01-01)}'::tstzmultirange),
  (2, 'Alice', 'BetaCorp', '{[2022-01-01, 2024-01-01)}'::tstzmultirange),
  (3, 'Bob',   'Acme',     '{[2021-01-01, 2023-01-01)}'::tstzmultirange),
  (4, 'Bob',   'BetaCorp', '{[2023-01-01, 2025-01-01)}'::tstzmultirange),
  (5, 'Carol', 'Acme',     '{[2020-06-01, 2024-06-01)}'::tstzmultirange),
  -- Diana left Acme, took a year off, came back: two disjoint spells.
  -- Picked specifically so that aggregating over Diana's rows yields a
  -- two-component multirange (rather than collapsing to a single span).
  (6, 'Diana', 'Acme',     '{[2018-01-01, 2019-01-01)}'::tstzmultirange),
  (7, 'Diana', 'Acme',     '{[2022-01-01, 2023-01-01)}'::tstzmultirange);

SELECT add_provenance('employments');

-- (value tstzmultirange, provenance UUID): auto-discovered by Studio's
-- /api/provenance_mappings since the column shape matches. The Eval
-- strip surfaces this as a mapping option whenever a tstzmultirange-
-- consuming semiring (currently only Temporal) is selected.
CREATE TABLE employments_validity AS
SELECT validity AS value, provsql AS provenance FROM employments;
SELECT remove_provenance('employments_validity');
CREATE INDEX ON employments_validity(provenance);

ANALYZE employments;
ANALYZE employments_validity;

\echo ''
\echo 'Loaded:'
\echo '  employments              5 rows, provenance-tracked'
\echo '  employments_validity     mapping table (value tstzmultirange, provenance uuid)'
\echo ''
\echo 'Try in ProvSQL Studio (Circuit mode):'
\echo '  -- 1. When was Acme actively employing at least someone?'
\echo '  --    Run the query below, then in the eval strip pick'
\echo '  --    Temporal (interval-union) + employments_validity, Run.'
\echo '  --    Expected: {[2020-01-01, 2024-06-01)} (union of three rows).'
\echo '  SELECT 1 AS acme_active FROM employments WHERE company = ''Acme'' GROUP BY 1;'
\echo ''
\echo '  -- 2. When was Alice ever employed (anywhere)?'
\echo '  --    Expected: {[2020-01-01, 2024-01-01)}.'
\echo '  SELECT 1 AS alice_active FROM employments WHERE person = ''Alice'' GROUP BY 1;'
\echo ''
\echo '  -- 3. When were Alice and Bob simultaneously employed at any company'
\echo '  --    (self-join, intersection of validities under the times semiring,'
\echo '  --    then union over the matching pairs)?'
\echo '  --    Expected: {[2021-01-01, 2024-01-01)}.'
\echo '  SELECT 1 AS overlap FROM employments e1, employments e2'
\echo '    WHERE e1.person = ''Alice'' AND e2.person = ''Bob'' GROUP BY 1;'
\echo ''
\echo '  -- 4. When did Diana ever work for Acme? Two disjoint spells, so the'
\echo '  --    result is a multirange with TWO components (not a single span).'
\echo '  --    Expected: {[2018-01-01, 2019-01-01), [2022-01-01, 2023-01-01)}.'
\echo '  SELECT 1 AS diana_at_acme FROM employments'
\echo '    WHERE person = ''Diana'' AND company = ''Acme'' GROUP BY 1;'
\echo ''
