-- ----------------------------------------------------------------------
-- test/bench/sum_cmp_bench.sql
--
-- Benchmark for the weighted-sum DP pre-pass
-- (provsql.cmp_probability_evaluation) on the canonical flat-table
-- HAVING-SUM query shape :
--
--   SELECT g, probability_evaluate(provenance())
--   FROM ss_t GROUP BY g HAVING sum(w) op c
--
-- where ss_t has n_rows tuples evenly across n_groups groups, each row
-- carrying an integer weight w.  The off-path (enumerate_valid_worlds ->
-- sum_dp) materialises the satisfying worlds as a DNF whose size grows
-- with the per-group size N ; the closed form replaces it with a
-- subset-sum DP over the reachable-sum range R (O(N x R)).  Thresholds
-- scale with N (c = 25 * per_grp) so the predicate stays non-degenerate
-- as the group grows.
--
-- The off/on comparison shapes keep per-group N <= 20 ; on-only shapes
-- reach N >= 30 where the off-path DNF is hopeless (the DP stays linear
-- in R, here bounded by 100 * N).  Each off-path shape runs under a 10s
-- statement_timeout ; off-path TIMEOUT rows are recorded with a
-- (skipped) diff_max so the report flags them.
--
-- Run :
--   createdb ssbench && psql ssbench -X -f test/bench/sum_cmp_bench.sql
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;
SELECT setseed(0.42);

DROP TABLE IF EXISTS ss_t CASCADE;
DROP TABLE IF EXISTS ss_results CASCADE;

CREATE TEMP TABLE ss_results(
  shape    text,
  n_rows   int,
  n_groups int,
  per_grp  int,
  op       text,
  c_thr    int,
  p_off_ms numeric,
  p_on_ms  numeric,
  speedup  numeric,
  diff_max numeric
);

-- Set up a fresh ss_t at the requested size, with per-row weight w in
-- [1, 100] and per-row Bernoulli probabilities in [0.3, 0.9].
CREATE OR REPLACE FUNCTION ss_setup(_n_rows int, _n_groups int)
  RETURNS void AS $$
BEGIN
  DROP TABLE IF EXISTS ss_t CASCADE;
  CREATE TABLE ss_t(id int, g text, w int);
  INSERT INTO ss_t
    SELECT i, 'g_' || (i % _n_groups), 1 + (random() * 99)::int
    FROM generate_series(1, _n_rows) i;
  PERFORM add_provenance('ss_t');
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM ss_t;
END
$$ LANGUAGE plpgsql;

-- ss_off / ss_on run the off- and on-path on the already-populated ss_t
-- for a given operator + threshold.  Called as SEPARATE top-level
-- statements so SESSION statement_timeout arms a fresh per-statement
-- timer.
CREATE OR REPLACE FUNCTION ss_off(_n_rows int, _n_groups int,
                                   _op text, _c int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_off numeric;
  per_grp int;
  shape text;
  q text;
  timed_out boolean := false;
BEGIN
  per_grp := _n_rows / _n_groups;
  shape := format('rows=%s groups=%s per_grp=%s sum %s %s',
                  _n_rows, _n_groups, per_grp, _op, _c);

  SET LOCAL provsql.cmp_probability_evaluation = off;
  q := format(
    'SELECT g, probability_evaluate(provenance()) AS p '
    'FROM ss_t GROUP BY g HAVING sum(w) %s %s', _op, _c);

  t0 := clock_timestamp();
  BEGIN
    EXECUTE 'CREATE TEMP TABLE ss_off AS ' || q;
  EXCEPTION WHEN query_canceled THEN timed_out := true;
  END;
  t1 := clock_timestamp();
  m_off := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  IF NOT timed_out THEN
    PERFORM remove_provenance('ss_off');
  END IF;

  INSERT INTO ss_results VALUES (
    shape, _n_rows, _n_groups, per_grp, _op, _c,
    m_off, NULL, NULL, NULL);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION ss_on(_n_rows int, _n_groups int,
                                  _op text, _c int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_on numeric;
  q text;
  off_ms numeric;
  off_exists boolean;
  max_diff numeric;
BEGIN
  SET LOCAL provsql.cmp_probability_evaluation = on;
  q := format(
    'SELECT g, probability_evaluate(provenance()) AS p '
    'FROM ss_t GROUP BY g HAVING sum(w) %s %s', _op, _c);

  t0 := clock_timestamp();
  EXECUTE 'CREATE TEMP TABLE ss_on AS ' || q;
  t1 := clock_timestamp();
  m_on := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'ss_off')
    INTO off_exists;
  IF off_exists THEN
    EXECUTE 'SELECT coalesce(max(ABS(o.p - n.p)), 0) '
            'FROM ss_off o JOIN ss_on n USING (g)' INTO max_diff;
  ELSE
    max_diff := NULL;
  END IF;

  PERFORM remove_provenance('ss_on');
  EXECUTE 'DROP TABLE ss_on';
  IF off_exists THEN EXECUTE 'DROP TABLE ss_off'; END IF;

  SELECT p_off_ms FROM ss_results
    WHERE n_rows = _n_rows AND n_groups = _n_groups
      AND op = _op AND c_thr = _c AND p_on_ms IS NULL
    LIMIT 1 INTO off_ms;
  IF off_ms IS NOT NULL THEN
    UPDATE ss_results
      SET p_on_ms = m_on,
          speedup = CASE WHEN max_diff IS NULL THEN NULL
                         ELSE round((off_ms / NULLIF(m_on, 0))::numeric, 2) END,
          diff_max = round(coalesce(max_diff, 0), 6)
      WHERE n_rows = _n_rows AND n_groups = _n_groups
        AND op = _op AND c_thr = _c AND p_on_ms IS NULL;
  ELSE
    INSERT INTO ss_results VALUES (
      format('rows=%s groups=%s per_grp=%s sum %s %s  [on only]',
             _n_rows, _n_groups, _n_rows / _n_groups, _op, _c),
      _n_rows, _n_groups, _n_rows / _n_groups, _op, _c,
      NULL, m_on, NULL, NULL);
  END IF;
END
$$ LANGUAGE plpgsql;


-- ----------------------------------------------------------------------
-- Bench grid.  Per-group N is the cost driver for the off-path DNF ;
-- the on-path DP is O(N x R) with R <= 100 * N.  Thresholds c = 25 * N
-- keep the predicate mid-range.
-- ----------------------------------------------------------------------
\set off_timeout '10s'

-- Small per-group (N = 10) : both paths fast, parity check.
SELECT ss_setup(40, 4);
SET statement_timeout = :'off_timeout'; SELECT ss_off(40, 4, '>=', 250); SET statement_timeout = 0; SELECT ss_on(40, 4, '>=', 250);
SET statement_timeout = :'off_timeout'; SELECT ss_off(40, 4, '<=', 250); SET statement_timeout = 0; SELECT ss_on(40, 4, '<=', 250);
SET statement_timeout = :'off_timeout'; SELECT ss_off(40, 4, '=',  250); SET statement_timeout = 0; SELECT ss_on(40, 4, '=',  250);

-- Medium per-group (N = 15).
SELECT ss_setup(60, 4);
SET statement_timeout = :'off_timeout'; SELECT ss_off(60, 4, '>=', 375); SET statement_timeout = 0; SELECT ss_on(60, 4, '>=', 375);
SET statement_timeout = :'off_timeout'; SELECT ss_off(60, 4, '<>', 375); SET statement_timeout = 0; SELECT ss_on(60, 4, '<>', 375);

-- Larger per-group (N = 18).
SELECT ss_setup(72, 4);
SET statement_timeout = :'off_timeout'; SELECT ss_off(72, 4, '>=', 450); SET statement_timeout = 0; SELECT ss_on(72, 4, '>=', 450);
SET statement_timeout = :'off_timeout'; SELECT ss_off(72, 4, '<=', 450); SET statement_timeout = 0; SELECT ss_on(72, 4, '<=', 450);

-- Threshold of blow-up (N = 20).
SELECT ss_setup(80, 4);
SET statement_timeout = :'off_timeout'; SELECT ss_off(80, 4, '>=', 500); SET statement_timeout = 0; SELECT ss_on(80, 4, '>=', 500);
SET statement_timeout = :'off_timeout'; SELECT ss_off(80, 4, '=',  500); SET statement_timeout = 0; SELECT ss_on(80, 4, '=',  500);

-- On-only shapes : per-group N too large for the off-path DNF ; the DP
-- stays linear in R (<= 100 * N).
SELECT ss_setup(120,  4);  SELECT ss_on(120,  4, '>=', 750);
SELECT ss_setup(400,  4);  SELECT ss_on(400,  4, '>=', 2500);
SELECT ss_setup(1000, 10); SELECT ss_on(1000, 10, '>=', 2500);

\echo
\echo '----------------------------------------------------------------------'
\echo 'HAVING SUM(w) op c : provsql.cmp_probability_evaluation off vs on'
\echo 'off-path capped at 10s ; TIMEOUT = sum_dp DNF path too slow'
\echo 'diff_max = max abs probability difference per group (~ 0 expected)'
\echo '[on only] rows : off-path skipped (per-group N too large)'
\echo '----------------------------------------------------------------------'
SELECT shape,
       CASE WHEN p_off_ms IS NULL THEN '(skipped)'
            WHEN diff_max IS NULL AND p_on_ms IS NOT NULL THEN
              'TIMEOUT (>' || p_off_ms::text || ' ms)'
            ELSE p_off_ms::text || ' ms' END  AS off_time,
       p_on_ms || ' ms' AS on_time,
       CASE WHEN p_off_ms IS NULL THEN '-'
            WHEN speedup IS NULL THEN 'inf'
            ELSE speedup::text || 'x' END     AS speedup,
       diff_max
FROM ss_results ORDER BY n_rows, n_groups, op, c_thr;

DROP TABLE ss_t CASCADE;
DROP FUNCTION ss_setup(int, int);
DROP FUNCTION ss_off(int, int, text, int);
DROP FUNCTION ss_on(int, int, text, int);
