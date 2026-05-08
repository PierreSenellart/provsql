-- Comprehensive eval-strip fixture for ProvSQL Studio's compiled-semiring
-- registry rework: exercises the new sub-categorised optgroups (Boolean
-- & symbolic, Lineage, Numeric, Interval-valued), the type-aware mapping
-- filter, and the carrier-dispatching `interval-union` family
-- (sr_temporal / sr_interval_num / sr_interval_int).
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_interval_union.sql
-- Then point the studio at <db> and try the suggested queries below.
--
-- Seeds two base tables:
--   * documents        : 6 rows, each with a publication validity range
--                        (tstzmultirange), a page-range citation
--                        (int4multirange), an evidence score in [0, 1]
--                        (float), an open-access flag (boolean), and a
--                        title (text).
--   * sensor_readings  : 5 rows, each with a numeric validity range
--                        (nummultirange) recording the parameter span
--                        over which a sensor's calibration is trusted.
--
-- And six (value, provenance uuid) mappings, one per carrier type the
-- eval-strip filters by:
--   * doc_titles               value text                : formula/why/which
--   * doc_open_access          value boolean             : sr_boolean
--   * doc_confidence           value double precision    : numeric group
--   * doc_publication_window   value tstzmultirange      : interval-union
--   * doc_pages                value int4multirange      : interval-union
--   * sensor_ranges            value nummultirange       : interval-union
--
-- All three multirange types require PostgreSQL 14+; the loader aborts
-- on older servers.

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

DO $$
BEGIN
  IF current_setting('server_version_num')::int < 140000 THEN
    RAISE EXCEPTION
      'interval-union demo requires PostgreSQL 14+ (multirange types).';
  END IF;
END $$;

SET TIME ZONE 'UTC';
SET datestyle = 'iso';
SET search_path TO public, provsql;

-- Drop in dependency order: mappings first (they have FK-shaped indexes
-- against the base tables' provsql column), then the base tables CASCADE
-- so leftover mapping copies from previous loads also go.
DROP TABLE IF EXISTS doc_titles;
DROP TABLE IF EXISTS doc_open_access;
DROP TABLE IF EXISTS doc_confidence;
DROP TABLE IF EXISTS doc_publication_window;
DROP TABLE IF EXISTS doc_pages;
DROP TABLE IF EXISTS sensor_ranges;
DROP TABLE IF EXISTS documents CASCADE;
DROP TABLE IF EXISTS sensor_readings CASCADE;

-- ─── base table 1 : documents ───────────────────────────────────────
-- A library / archival fixture. Each row records a published artifact
-- with five distinct annotations, each corresponding to a different
-- compiled-semiring family in the eval strip.

CREATE TABLE documents (
  id                 SERIAL PRIMARY KEY,
  title              TEXT             NOT NULL,
  -- Multi-component ranges where it matters: see doc 5 (republished)
  -- and doc 6 (cited on disjoint page intervals across two volumes).
  publication_window tstzmultirange   NOT NULL,
  pages              int4multirange   NOT NULL,
  evidence           DOUBLE PRECISION NOT NULL CHECK (evidence BETWEEN 0 AND 1),
  is_open_access     BOOLEAN          NOT NULL
);

INSERT INTO documents
  (title, publication_window, pages, evidence, is_open_access) VALUES
  ('Provenance Semirings',
   '{[2007-06-01, 2008-06-01)}'::tstzmultirange,
   '{[31, 41)}'::int4multirange,
   0.95, TRUE),
  ('Bag Semantics for Provenance',
   '{[2010-03-01, 2011-03-01)}'::tstzmultirange,
   '{[201, 213)}'::int4multirange,
   0.80, TRUE),
  ('Why and Where: A Survey',
   '{[2009-09-01, 2010-09-01)}'::tstzmultirange,
   '{[1, 27)}'::int4multirange,
   0.70, FALSE),
  ('A Top-Down Approach to Probabilistic DBs',
   '{[2013-01-01, 2014-01-01)}'::tstzmultirange,
   '{[100, 130)}'::int4multirange,
   0.60, FALSE),
  -- Republished in a second venue: two disjoint validity windows so
  -- the temporal interval-union over the row aggregates to a multirange
  -- with TWO components, not a single span.
  ('Lineage in Data Warehouses',
   '{[2000-04-01, 2001-04-01), [2018-01-01, 2019-01-01)}'::tstzmultirange,
   '{[155, 187)}'::int4multirange,
   0.55, TRUE),
  -- Anthology entry cited across two non-contiguous page ranges.
  ('Provenance for Database Transformations',
   '{[2008-06-01, 2009-06-01)}'::tstzmultirange,
   '{[12, 18), [40, 42)}'::int4multirange,
   0.40, FALSE);

SELECT add_provenance('documents');

-- ─── base table 2 : sensor_readings ─────────────────────────────────
-- Numeric-interval companion. Each row asserts that a sensor's reading
-- is calibrated over a parameter range (e.g., `x ∈ [3.2, 7.8)`); join
-- queries compute jointly-valid ranges for sensor fusion.

CREATE TABLE sensor_readings (
  id             SERIAL PRIMARY KEY,
  sensor         TEXT          NOT NULL,
  validity_range nummultirange NOT NULL
);

INSERT INTO sensor_readings (sensor, validity_range) VALUES
  ('thermistor-A', '{[3.2, 7.8)}'::nummultirange),
  ('thermistor-B', '{[5.0, 9.1)}'::nummultirange),
  -- Two disjoint calibration ranges (covers low-temp and high-temp
  -- regimes, with a gap in between) : a + over alt_sensor's rows
  -- yields a two-component multirange.
  ('alt_sensor',   '{[-2.0, 1.5)}'::nummultirange),
  ('alt_sensor',   '{[8.5, 12.0)}'::nummultirange),
  ('reference',    '{[0.0, 10.0)}'::nummultirange);

SELECT add_provenance('sensor_readings');

-- ─── (value, provenance) mappings : one per carrier type ────────────
-- Each is auto-discovered by Studio's /api/provenance_mappings (column
-- shape `(value <T>, provenance uuid)`). The eval-strip's type-aware
-- filter then surfaces only the mappings whose base type matches the
-- selected semiring.

-- text → formula / why / which (polymorphic) and any custom wrapper
-- whose return type is text.
CREATE TABLE doc_titles AS
  SELECT title AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('doc_titles');
CREATE INDEX ON doc_titles(provenance);

-- boolean → sr_boolean (Boolean & symbolic group).
CREATE TABLE doc_open_access AS
  SELECT is_open_access AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('doc_open_access');
CREATE INDEX ON doc_open_access(provenance);

-- double precision in [0, 1] → counting / tropical / viterbi /
-- lukasiewicz (Numeric group). Viterbi computes the highest-evidence
-- contributing document; Łukasiewicz uses the bounded-conjunction
-- t-norm so chains stay non-zero longer than the Viterbi product.
CREATE TABLE doc_confidence AS
  SELECT evidence AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('doc_confidence');
CREATE INDEX ON doc_confidence(provenance);

-- tstzmultirange → interval-union → sr_temporal.
CREATE TABLE doc_publication_window AS
  SELECT publication_window AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('doc_publication_window');
CREATE INDEX ON doc_publication_window(provenance);

-- int4multirange → interval-union → sr_interval_int.
CREATE TABLE doc_pages AS
  SELECT pages AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('doc_pages');
CREATE INDEX ON doc_pages(provenance);

-- nummultirange → interval-union → sr_interval_num.
CREATE TABLE sensor_ranges AS
  SELECT validity_range AS value, provsql AS provenance FROM sensor_readings;
SELECT remove_provenance('sensor_ranges');
CREATE INDEX ON sensor_ranges(provenance);

ANALYZE documents;
ANALYZE sensor_readings;
ANALYZE doc_titles;
ANALYZE doc_open_access;
ANALYZE doc_confidence;
ANALYZE doc_publication_window;
ANALYZE doc_pages;
ANALYZE sensor_ranges;

\echo ''
\echo 'Loaded:'
\echo '  documents                  6 rows, provenance-tracked'
\echo '  sensor_readings            5 rows, provenance-tracked'
\echo '  doc_titles                 mapping (text)'
\echo '  doc_open_access            mapping (boolean)'
\echo '  doc_confidence             mapping (double precision)'
\echo '  doc_publication_window     mapping (tstzmultirange)'
\echo '  doc_pages                  mapping (int4multirange)'
\echo '  sensor_ranges              mapping (nummultirange)'
\echo ''
\echo 'Try in ProvSQL Studio (Circuit mode). Open the eval-strip semiring'
\echo 'dropdown to see the four sub-optgroups (Boolean & symbolic, Lineage,'
\echo 'Numeric, Interval-valued); the mapping picker narrows automatically'
\echo 'to the value types each group accepts.'
\echo ''
\echo '  -- Q1. Run any SELECT against documents to get a row UUID, then'
\echo '  --     pick "Interval union (multirange)" + a multirange mapping.'
\echo '  --     The same UI option dispatches to a different kernel by type:'
\echo '  --       doc_publication_window     → sr_temporal'
\echo '  --       doc_pages                  → sr_interval_int'
\echo '  --       sensor_ranges              → sr_interval_num'
\echo '  SELECT * FROM documents;'
\echo ''
\echo '  -- Q2. Aggregate over the whole catalog. Click the resulting UUID,'
\echo '  --     pick interval-union + doc_publication_window:'
\echo '  --       expected: a multirange covering the catalog''s spans,'
\echo '  --       including the disjoint pair from Lineage in Data Warehouses.'
\echo '  SELECT 1 AS k FROM documents GROUP BY 1;'
\echo ''
\echo '  -- Q3. Same root, then interval-union + doc_pages:'
\echo '  --       expected: a multirange of the cited page intervals,'
\echo '  --       including [12, 18) ∪ [40, 42) from the anthology entry.'
\echo ''
\echo '  -- Q4. Same root, then Viterbi + doc_confidence:'
\echo '  --       expected: 0.95 (the highest-evidence document''s score).'
\echo '  --     Then switch to Łukasiewicz : same result for ⊕=max,'
\echo '  --     but bounded conjunction kicks in for ⊗ on joins.'
\echo ''
\echo '  -- Q5. Sensor fusion : aggregate sensor_readings, then'
\echo '  --     interval-union + sensor_ranges:'
\echo '  --       expected: {[-2.0, 12.0)} after merging adjacent /'
\echo '  --       overlapping calibration ranges; alt_sensor''s two'
\echo '  --       disjoint regimes get folded into the unified envelope.'
\echo '  SELECT 1 AS k FROM sensor_readings GROUP BY 1;'
\echo ''
\echo '  -- Q6. Type-filter demo : pick "Boolean" in the eval strip with'
\echo '  --     no mapping selected; the picker should show only'
\echo '  --     doc_open_access (the sole boolean mapping). Switching to'
\echo '  --     "Counting" widens it to doc_confidence (numeric).'
\echo '  --     Switching to "Interval union" narrows to the three'
\echo '  --     multirange-typed mappings.'
\echo ''
