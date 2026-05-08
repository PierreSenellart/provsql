-- Small enum-lattice fixture for ProvSQL Studio's user-enum compiled
-- semirings (sr_minmax / sr_maxmin).
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_minmax.sql
-- Then point the studio at <db> and exercise the suggested queries below
-- by copy-pasting them into the querybox in Circuit mode.
--
-- Seeds two parallel use cases, one per shape of the min-max algebra:
--
--   1. security clearance (sr_minmax over `clearance_level`):
--        ⊕ = enum-min (alternatives: least sensitive label wins)
--        ⊗ = enum-max (joins: most sensitive label wins)
--      Carrier: public < internal < confidential < secret < classified
--      Tables: documents, document_clearance (mapping)
--
--   2. evidence trust (sr_maxmin over `trust_level`):
--        ⊕ = enum-max (alternatives: most permissive label wins)
--        ⊗ = enum-min (joins: strictest label wins)
--      Carrier: untrusted < low < medium < high < verified
--      Tables: sources, source_trust (mapping)
--
-- Both enums are user-defined: the studio's eval strip surfaces sr_minmax
-- and sr_maxmin only when at least one enum-typed mapping is present.

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

SET search_path TO public, provsql;

DROP TABLE IF EXISTS document_clearance;
DROP TABLE IF EXISTS documents CASCADE;
DROP TABLE IF EXISTS source_trust;
DROP TABLE IF EXISTS sources CASCADE;
DROP TYPE  IF EXISTS clearance_level;
DROP TYPE  IF EXISTS trust_level;

CREATE TYPE clearance_level AS ENUM
  ('public', 'internal', 'confidential', 'secret', 'classified');

CREATE TYPE trust_level AS ENUM
  ('untrusted', 'low', 'medium', 'high', 'verified');

------------------------------------------------------------------ documents
CREATE TABLE documents (
  doc_id    INTEGER NOT NULL,
  topic     TEXT    NOT NULL,
  clearance clearance_level NOT NULL
);

INSERT INTO documents (doc_id, topic, clearance) VALUES
  -- Multi-author dossiers: same doc_id pairs up under a self-join, ⊗ takes
  -- the most sensitive author classification.
  (1, 'budget',   'public'),
  (1, 'budget',   'internal'),
  (2, 'roadmap',  'internal'),
  (2, 'roadmap',  'confidential'),
  (3, 'incident', 'confidential'),
  (3, 'incident', 'secret'),
  (3, 'incident', 'classified'),
  -- Single-author docs: nothing to join, the ⊗ identity falls through.
  (4, 'minutes',  'public'),
  (5, 'leak',     'classified');

SELECT add_provenance('documents');

CREATE TABLE document_clearance AS
SELECT clearance AS value, provsql AS provenance FROM documents;
SELECT remove_provenance('document_clearance');
CREATE INDEX ON document_clearance(provenance);

------------------------------------------------------------------- sources
CREATE TABLE sources (
  src_id  INTEGER NOT NULL,
  claim   TEXT    NOT NULL,
  trust   trust_level NOT NULL
);

INSERT INTO sources (src_id, claim, trust) VALUES
  -- Multiple corroborating sources for the same claim: + takes the most
  -- permissive trust, joins (over a self-join below) take the strictest.
  (1, 'breach-2026', 'verified'),
  (1, 'breach-2026', 'high'),
  (1, 'breach-2026', 'medium'),
  (2, 'rumor-q2',    'low'),
  (2, 'rumor-q2',    'untrusted'),
  (3, 'leak-payroll','high'),
  (3, 'leak-payroll','high'),
  (4, 'tip-anon',    'untrusted'),
  (5, 'audit-2025',  'verified'),
  (5, 'audit-2025',  'verified');

SELECT add_provenance('sources');

CREATE TABLE source_trust AS
SELECT trust AS value, provsql AS provenance FROM sources;
SELECT remove_provenance('source_trust');
CREATE INDEX ON source_trust(provenance);

ANALYZE documents;
ANALYZE document_clearance;
ANALYZE sources;
ANALYZE source_trust;

\echo ''
\echo 'Loaded:'
\echo '  documents             9 rows, provenance-tracked'
\echo '  document_clearance    mapping (value clearance_level, provenance uuid)'
\echo '  sources               10 rows, provenance-tracked'
\echo '  source_trust          mapping (value trust_level, provenance uuid)'
\echo ''
\echo 'Try in ProvSQL Studio (Circuit mode):'
\echo '  -- Each query below produces one row with a provsql UUID. Click'
\echo '  -- the UUID to see the circuit, then in the eval strip pick the'
\echo '  -- semiring + matching mapping + Run.'
\echo ''
\echo '  -- 1. SECURITY SHAPE (sr_minmax + document_clearance).'
\echo '  --    Two-author join over doc_id: ⊗ = enum-max picks the more'
\echo '  --    sensitive author per pair, ⊕ = enum-min collapses across'
\echo '  --    pairs to the least sensitive joined classification.'
\echo '  --      doc 1 (public, internal)                       -> internal'
\echo '  --      doc 2 (internal, confidential)                 -> confidential'
\echo '  --      doc 3 (confidential, secret, classified)       -> secret'
\echo '  --    Across the three docs, ⊕ takes the min: -> internal.'
\echo '  --    Eval strip: "Min-max (security shape)" + document_clearance.'
\echo '  SELECT 1 AS k FROM documents d1, documents d2'
\echo '   WHERE d1.doc_id = d2.doc_id AND d1.ctid < d2.ctid'
\echo '   GROUP BY 1;'
\echo ''
\echo '  -- 2. FUZZY / TRUST SHAPE (sr_maxmin + source_trust).'
\echo '  --    Self-join over claim: ⊗ = enum-min keeps the weakest source'
\echo '  --    in each pair, ⊕ = enum-max promotes the best-corroborated'
\echo '  --    claim across the lot.'
\echo '  --      breach-2026 (3 rows -> 3 pairs, mins high/medium/medium) -> high'
\echo '  --      rumor-q2    (low, untrusted)                              -> untrusted'
\echo '  --      leak-payroll(high, high)                                  -> high'
\echo '  --      audit-2025  (verified, verified)                          -> verified'
\echo '  --    Across the four claims, ⊕ takes the max: -> verified.'
\echo '  --    Eval strip: "Max-min (enum fuzzy / trust)" + source_trust.'
\echo '  SELECT 1 AS k FROM sources s1, sources s2'
\echo '   WHERE s1.claim = s2.claim AND s1.ctid < s2.ctid'
\echo '   GROUP BY 1;'
\echo ''
\echo '  -- 3. CONTRAST: same circuit, both shapes.'
\echo '  --    Run query (1) with sr_minmax (security) and sr_maxmin (fuzzy)'
\echo '  --    on the same document_clearance mapping to see the duality.'
\echo '  --    The doc-3 fan-in (3 rows -> 3 pairs) drives both:'
\echo '  --      sr_minmax over the 3 docs -> internal  (least sensitive overall)'
\echo '  --      sr_maxmin over the 3 docs -> secret    (most permissive overall)'
\echo ''
