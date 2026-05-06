-- Synthetic big-data fixture for ProvSQL Studio rendering / circuit stress tests.
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_big.sql
-- Then point the studio at <db> and exercise studio/scripts/big_demo_queries.sql
-- by copy-pasting individual queries into the querybox.
--
-- The fixture seeds three tables:
--   * bench_events                : 50000 rows, 8 user columns, provenance-tracked.
--   * bench_dim                   : 4 rows, certain (untracked) dimension table.
--   * bench_severity_mapping      : (value INTEGER, provenance UUID) mapping
--                                   discovered by the eval-strip dropdown.
-- Plus per-row probabilities on bench_events (decreasing with severity).

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

SET search_path TO public, provsql;

-- Reproducible RNG so circuit shapes are stable across reloads.
SELECT setseed(0.42);

DROP TABLE IF EXISTS bench_severity_mapping;
DROP TABLE IF EXISTS bench_events CASCADE;
DROP TABLE IF EXISTS bench_dim CASCADE;

-- ~50k rows, 8 user columns. Mix of joinable / filterable / aggregatable.
CREATE TABLE bench_events (
  id        INTEGER PRIMARY KEY,
  ts        TIMESTAMPTZ NOT NULL,
  region    TEXT        NOT NULL,
  kind      TEXT        NOT NULL,
  severity  INTEGER     NOT NULL,            -- 0..4
  payload   TEXT        NOT NULL,
  tags      TEXT[]      NOT NULL,
  cost      NUMERIC(10,2) NOT NULL
);

INSERT INTO bench_events
SELECT
  i,
  TIMESTAMPTZ '2026-01-01' + (i || ' minutes')::INTERVAL,
  (ARRAY['EU','US','APAC','LATAM'])[1 + (i % 4)],
  (ARRAY['ack','warn','err','info','debug'])[1 + (i % 5)],
  i % 5,
  md5(i::TEXT),
  ARRAY['t' || (i % 10), 't' || (i % 17)],
  ((i % 1000) / 10.0)::NUMERIC(10,2)
FROM generate_series(1, 50000) g(i);

-- Small dimension table for join experiments. Untracked: behaves as
-- "certain" data (the case-study convention; cf. CS5 §5).
CREATE TABLE bench_dim (
  region   TEXT PRIMARY KEY,
  display  TEXT NOT NULL,
  weight   NUMERIC(4,2) NOT NULL
);
INSERT INTO bench_dim VALUES
  ('EU',    'Europe',         1.10),
  ('US',    'United States',  1.00),
  ('APAC',  'Asia-Pacific',   0.90),
  ('LATAM', 'Latin America',  0.80);

-- Track provenance on bench_events.
SELECT add_provenance('bench_events');

-- Materialize provenance for every row, attach probabilities, and seed the
-- severity mapping in one DO block so the input gates are created exactly
-- once and shared across all three operations.
DO $$
BEGIN
  -- Per-row probability decreasing with severity (sev=0 -> 0.50, sev=4 -> 0.10).
  PERFORM set_prob(provenance(), 0.50 - severity*0.10)
  FROM bench_events;
END $$;

-- (value INTEGER, provenance UUID): auto-discovered by the eval strip's
-- /api/provenance_mappings endpoint. Useful for the counting / formula /
-- why semirings.
CREATE TABLE bench_severity_mapping AS
SELECT severity AS value, provenance() AS provenance
FROM bench_events;

ANALYZE bench_events;
ANALYZE bench_dim;
ANALYZE bench_severity_mapping;

\echo ''
\echo 'Loaded:'
\echo '  bench_events             50000 rows, provenance-tracked, with probabilities'
\echo '  bench_dim                4 rows (certain, untracked)'
\echo '  bench_severity_mapping   provenance mapping over severity (INTEGER)'
\echo ''
\echo 'Try studio/scripts/big_demo_queries.sql for canned big-table / big-circuit queries.'
