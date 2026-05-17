-- Demo fixture for ProvSQL Studio exploring the safe-query
-- (Boolean-rewrite) optimisation under provsql.boolean_provenance.
--
-- Run once against a fresh database:
--   createdb sqdemo && psql sqdemo -f studio/scripts/load_demo_safe_query.sql
--
-- Then point Studio at sqdemo, switch to Circuit mode, and run the
-- queries listed in the COMMENTS at the bottom of this file by
-- copy-pasting them into the querybox.  Toggle
-- provsql.boolean_provenance between ON and OFF (Config panel
-- → GUC overrides, or `SET` in the querybox) to compare the two
-- circuit shapes side by side.
--
-- Five tracked tables form a small hierarchical CQ playground:
--   * sq_a(x, y, z) -- 60 rows (5x * 4y * 3z)
--   * sq_b(x, y, z) -- mirror of A, independent provsql leaves
--   * sq_c(x, y)    -- 20 rows
--   * sq_d(x, w)    -- 15 rows
--   * sq_e(x, w)    -- mirror of D
-- One BID-tracked table for the block-correlated path:
--   * sq_bid(x, k)  -- 5 blocks of 4 rows; block_key = x
-- Plus a `name`-style mapping discovered by Studio's eval-strip
-- dropdown for the formula / how / which semirings:
--   * sq_a_label, sq_b_label, sq_c_label, sq_d_label, sq_e_label

\set ECHO none
\pset format unaligned

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;

SELECT setseed(0.42);

DROP TABLE IF EXISTS sq_a_label, sq_b_label, sq_c_label,
                     sq_d_label, sq_e_label;
DROP TABLE IF EXISTS sq_a, sq_b, sq_c, sq_d, sq_e, sq_bid CASCADE;

-- Atoms.
CREATE TABLE sq_a AS
  SELECT x, y, z, format('A(%s,%s,%s)', x, y, z) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 4) y,
         generate_series(1, 3) z;

CREATE TABLE sq_b AS
  SELECT x, y, z, format('B(%s,%s,%s)', x, y, z) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 4) y,
         generate_series(1, 3) z;

CREATE TABLE sq_c AS
  SELECT x, y, format('C(%s,%s)', x, y) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 4) y;

CREATE TABLE sq_d AS
  SELECT x, w, format('D(%s,%s)', x, w) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 3) w;

CREATE TABLE sq_e AS
  SELECT x, w, format('E(%s,%s)', x, w) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 3) w;

CREATE TABLE sq_bid AS
  SELECT x, k, format('BID(%s,%s)', x, k) AS lbl
    FROM generate_series(1, 5) x,
         generate_series(1, 4) k;

-- Provenance.
SELECT add_provenance('sq_a');
SELECT add_provenance('sq_b');
SELECT add_provenance('sq_c');
SELECT add_provenance('sq_d');
SELECT add_provenance('sq_e');
SELECT repair_key('sq_bid', 'x');

-- Random probabilities so the comparison is non-trivial.
DO $$ BEGIN
  PERFORM set_prob(provsql, 0.3 + 0.5 * random()) FROM sq_a;
  PERFORM set_prob(provsql, 0.3 + 0.5 * random()) FROM sq_b;
  PERFORM set_prob(provsql, 0.3 + 0.5 * random()) FROM sq_c;
  PERFORM set_prob(provsql, 0.3 + 0.5 * random()) FROM sq_d;
  PERFORM set_prob(provsql, 0.3 + 0.5 * random()) FROM sq_e;
END $$;

-- Label mappings (Studio's eval strip will pick these up).
SELECT create_provenance_mapping('sq_a_label', 'sq_a', 'lbl');
SELECT create_provenance_mapping('sq_b_label', 'sq_b', 'lbl');
SELECT create_provenance_mapping('sq_c_label', 'sq_c', 'lbl');
SELECT create_provenance_mapping('sq_d_label', 'sq_d', 'lbl');
SELECT create_provenance_mapping('sq_e_label', 'sq_e', 'lbl');

\echo
\echo '----------------------------------------------------------------------'
\echo 'sqdemo loaded.  Open Studio against this database and paste the'
\echo 'queries below (one at a time) into the querybox.  Toggle the GUC'
\echo 'with `SET provsql.boolean_provenance = on;` (or off) before each.'
\echo '----------------------------------------------------------------------'
\echo
\echo '-- (Q1) 2-atom hierarchical CQ over (x, y, z).'
\echo 'SELECT a.x, provenance()'
\echo '  FROM sq_a a, sq_b b'
\echo ' WHERE a.x = b.x AND a.y = b.y AND a.z = b.z'
\echo ' GROUP BY a.x;'
\echo
\echo '-- (Q2) 3-atom multi-level: A, B share (x,y,z), C shares (x,y).'
\echo 'SELECT a.x, provenance()'
\echo '  FROM sq_a a, sq_b b, sq_c c'
\echo ' WHERE a.x = b.x AND a.x = c.x'
\echo '   AND a.y = b.y AND a.y = c.y AND a.z = b.z'
\echo ' GROUP BY a.x;'
\echo
\echo '-- (Q3) 2 components: (A,B on x,y,z) x (D,E on x,w).'
\echo 'SELECT a.x, provenance()'
\echo '  FROM sq_a a, sq_b b, sq_d d, sq_e e'
\echo ' WHERE a.x = b.x AND a.x = d.x AND a.x = e.x'
\echo '   AND a.y = b.y AND a.z = b.z AND d.w = e.w'
\echo ' GROUP BY a.x;'
\echo
\echo '-- (Q4) Bridge case: 5 atoms; y bridges {A,B} and {C}.'
\echo 'SELECT a.x, provenance()'
\echo '  FROM sq_a a, sq_b b, sq_c c, sq_d d, sq_e e'
\echo ' WHERE a.x = b.x AND a.x = c.x AND a.x = d.x AND a.x = e.x'
\echo '   AND a.y = b.y AND a.y = c.y AND a.z = b.z AND d.w = e.w'
\echo ' GROUP BY a.x;'
\echo
\echo '-- (Q5) BID atom joined with TID atom A on x.'
\echo 'SELECT bb.x, provenance()'
\echo '  FROM sq_bid bb, sq_a a'
\echo ' WHERE bb.x = a.x'
\echo ' GROUP BY bb.x;'
\echo
\echo 'In Circuit mode, look for `gate_assumed_boolean` wrapping the per-row'
\echo 'root when the GUC is ON, and the absence of the wrapper when OFF.'
\echo 'The eval strip exposes probability_evaluate / sr_formula / etc. to'
\echo 'compare semirings side by side.'
