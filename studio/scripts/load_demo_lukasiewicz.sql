-- Small fuzzy-logic fixture for ProvSQL Studio's Łukasiewicz compiled
-- semiring (sr_lukasiewicz).
--
-- Run once against a fresh database:
--   psql -d <db> -f studio/scripts/load_demo_lukasiewicz.sql
-- Then point the studio at <db> and exercise the suggested queries below
-- by copy-pasting them into the querybox in Circuit mode.
--
-- Seeds two tables:
--   * symptoms             : 12 rows, provenance-tracked. Each row records
--                            a (patient, symptom, evidence) triple where
--                            `evidence` is a degree-of-support value in
--                            [0, 1] (0 = no evidence, 1 = certain).
--   * symptoms_evidence    : (value float, provenance UUID) mapping
--                            auto-discovered by the Eval strip.
--
-- Łukasiewicz semantics (fuzzy / multi-valued logic):
--   ⊕ (plus)  : max(a, b)              -- alternative evidence
--   ⊗ (times) : max(a + b - 1, 0)      -- bounded conjunction
--   ⊖ (monus) : 0 if a ≤ b else a
--   𝟘 = 0, 𝟙 = 1
--
-- The bounded-conjunction t-norm makes ⊗ stay crisp at 1 (1.0 ⊗ x = x)
-- while degrading gracefully on weaker pairs. Compare with Viterbi
-- (a · b) which collapses faster on long chains of mid-range values.

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

SET search_path TO public, provsql;

DROP TABLE IF EXISTS symptoms_evidence;
DROP TABLE IF EXISTS symptoms CASCADE;

CREATE TABLE symptoms (
  patient_id INTEGER NOT NULL,
  symptom    TEXT    NOT NULL,
  evidence   FLOAT   NOT NULL CHECK (evidence BETWEEN 0 AND 1)
);

INSERT INTO symptoms (patient_id, symptom, evidence) VALUES
  -- Alice: strong, fairly crisp evidence on both flu symptoms.
  (1, 'fever',    0.9),
  (1, 'cough',    0.8),
  (1, 'fatigue',  0.4),
  -- Bob: clear-cut, both at 1.0.
  (2, 'fever',    1.0),
  (2, 'cough',    1.0),
  -- Carol: middling evidence; bounded-difference ⊗ should drop sharply.
  (3, 'fever',    0.6),
  (3, 'cough',    0.5),
  -- Dan: right at the t-norm boundary (0.5 + 0.5 - 1 = 0).
  (4, 'fever',    0.5),
  (4, 'cough',    0.5),
  -- Eve: only one of the two flu symptoms, plus an unrelated one.
  (5, 'fatigue',  0.7),
  (5, 'cough',    0.7),
  -- Frank: two readings of the same symptom on different visits;
  -- ⊕ (max) over them is the strongest single observation.
  (6, 'cough',    0.6),
  (6, 'cough',    0.8);

SELECT add_provenance('symptoms');

-- (value float, provenance UUID): auto-discovered by Studio's
-- /api/provenance_mappings since the column shape matches. The Eval
-- strip's "Łukasiewicz (fuzzy)" entry picks this up automatically.
CREATE TABLE symptoms_evidence AS
SELECT evidence AS value, provsql AS provenance FROM symptoms;
SELECT remove_provenance('symptoms_evidence');
CREATE INDEX ON symptoms_evidence(provenance);

ANALYZE symptoms;
ANALYZE symptoms_evidence;

\echo ''
\echo 'Loaded:'
\echo '  symptoms              12 rows, provenance-tracked'
\echo '  symptoms_evidence     mapping table (value float, provenance uuid)'
\echo ''
\echo 'Try in ProvSQL Studio (Circuit mode):'
\echo '  -- Each query below produces one row per matching patient with a'
\echo '  -- provsql UUID. Click the UUID to see the circuit, then in the'
\echo '  -- eval strip pick "Łukasiewicz (fuzzy)" + symptoms_evidence + Run.'
\echo ''
\echo '  -- 1. Patients with EVIDENCE OF FEVER (single-symptom).'
\echo '  --    For each patient, the sole `fever` evidence value is also'
\echo '  --    the Łukasiewicz score. Try Alice (0.9), Bob (1.0), Carol (0.6).'
\echo '  SELECT patient_id FROM symptoms WHERE symptom = ''fever'';'
\echo ''
\echo '  -- 2. Patients with FEVER AND COUGH (conjunction via self-join).'
\echo '  --    ⊗ = max(a + b - 1, 0), so:'
\echo '  --      Alice (0.9, 0.8) -> 0.7   (high-evidence pair)'
\echo '  --      Bob   (1.0, 1.0) -> 1.0   (crisp truth preserved)'
\echo '  --      Carol (0.6, 0.5) -> 0.1   (marginal, drops sharply)'
\echo '  --      Dan   (0.5, 0.5) -> 0     (right at the t-norm boundary)'
\echo '  --    Eve has no fever, so she does not appear.'
\echo '  SELECT f.patient_id FROM symptoms f, symptoms c'
\echo '   WHERE f.patient_id = c.patient_id'
\echo '     AND f.symptom = ''fever'' AND c.symptom = ''cough'';'
\echo ''
\echo '  -- 3. Patients with EVIDENCE OF COUGH (alternative readings).'
\echo '  --    GROUP BY collapses each patient''s rows into one provenance'
\echo '  --    circuit: a `plus` gate over their cough input gates.'
\echo '  --    ⊕ = max, so Frank''s repeated readings (0.6, 0.8) collapse'
\echo '  --    to 0.8 -- the strongest single observation, not the sum.'
\echo '  --    Compare with Counting (which adds them) to see the contrast.'
\echo '  SELECT patient_id FROM symptoms WHERE symptom = ''cough'''
\echo '   GROUP BY patient_id;'
\echo ''
