-- ----------------------------------------------------------------------
-- test/bench/count_cmp_bench.sql
--
-- Benchmark for the Poisson-binomial pre-pass
-- (provsql.cmp_probability_evaluation) on the canonical flat-table
-- HAVING-COUNT query shape :
--
--   SELECT g, probability_evaluate(provenance())
--   FROM cc_t GROUP BY g HAVING count(*) op c
--
-- where cc_t has n_rows tuples evenly across n_groups groups.  The
-- blow-up point for the unoptimised path is k ~ N/2 with N the
-- per-group row count : count_enum emits binom(N, k) minimal-generator
-- clauses, which the downstream probability evaluator then has to
-- compile or solve.  Closed-form Poisson-binomial DP replaces all of
-- that with O(N * C) arithmetic.
--
-- Each shape runs the off-path under a 10s statement_timeout
-- (top-level SET arms the per-statement timer) and the on-path
-- without ; off-path TIMEOUT rows are recorded with a (skipped)
-- diff_max so the report flags them clearly.
--
-- Run :
--   createdb ccbench && psql ccbench -X -f test/bench/count_cmp_bench.sql
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;
SELECT setseed(0.42);

DROP TABLE IF EXISTS cc_t CASCADE;
DROP TABLE IF EXISTS cc_results CASCADE;

CREATE TEMP TABLE cc_results(
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

-- Set up a fresh cc_t at the requested size, with per-row Bernoulli
-- probabilities in [0.3, 0.9].
CREATE OR REPLACE FUNCTION cc_setup(_n_rows int, _n_groups int)
  RETURNS void AS $$
BEGIN
  DROP TABLE IF EXISTS cc_t CASCADE;
  CREATE TABLE cc_t(id int, g text);
  INSERT INTO cc_t
    SELECT i, 'g_' || (i % _n_groups)
    FROM generate_series(1, _n_rows) i;
  PERFORM add_provenance('cc_t');
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM cc_t;
END
$$ LANGUAGE plpgsql;

-- cc_off / cc_on run the off- and on-path on the already-populated
-- cc_t for a given operator + threshold.  Called as SEPARATE
-- top-level statements so SESSION statement_timeout arms a fresh
-- per-statement timer.  No warmup : when the warmup itself times out
-- there is no measurement to record.
CREATE OR REPLACE FUNCTION cc_off(_n_rows int, _n_groups int,
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
  shape := format('rows=%s groups=%s per_grp=%s count %s %s',
                  _n_rows, _n_groups, per_grp, _op, _c);

  SET LOCAL provsql.cmp_probability_evaluation = off;
  q := format(
    'SELECT g, probability_evaluate(provenance()) AS p '
    'FROM cc_t GROUP BY g HAVING count(*) %s %s', _op, _c);

  t0 := clock_timestamp();
  BEGIN
    EXECUTE 'CREATE TEMP TABLE cc_off AS ' || q;
  EXCEPTION WHEN query_canceled THEN timed_out := true;
  END;
  t1 := clock_timestamp();
  m_off := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  IF NOT timed_out THEN
    PERFORM remove_provenance('cc_off');
  END IF;

  INSERT INTO cc_results VALUES (
    shape, _n_rows, _n_groups, per_grp, _op, _c,
    m_off, NULL, NULL, NULL);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION cc_on(_n_rows int, _n_groups int,
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
    'FROM cc_t GROUP BY g HAVING count(*) %s %s', _op, _c);

  t0 := clock_timestamp();
  EXECUTE 'CREATE TEMP TABLE cc_on AS ' || q;
  t1 := clock_timestamp();
  m_on := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'cc_off')
    INTO off_exists;
  IF off_exists THEN
    EXECUTE 'SELECT coalesce(max(ABS(o.p - n.p)), 0) '
            'FROM cc_off o JOIN cc_on n USING (g)' INTO max_diff;
  ELSE
    max_diff := NULL;
  END IF;

  PERFORM remove_provenance('cc_on');
  EXECUTE 'DROP TABLE cc_on';
  IF off_exists THEN EXECUTE 'DROP TABLE cc_off'; END IF;

  SELECT p_off_ms FROM cc_results
    WHERE n_rows = _n_rows AND n_groups = _n_groups
      AND op = _op AND c_thr = _c AND p_on_ms IS NULL
    LIMIT 1 INTO off_ms;
  IF off_ms IS NOT NULL THEN
    UPDATE cc_results
      SET p_on_ms = m_on,
          speedup = CASE WHEN max_diff IS NULL THEN NULL
                         ELSE round((off_ms / NULLIF(m_on, 0))::numeric, 2) END,
          diff_max = round(coalesce(max_diff, 0), 6)
      WHERE n_rows = _n_rows AND n_groups = _n_groups
        AND op = _op AND c_thr = _c AND p_on_ms IS NULL;
  ELSE
    INSERT INTO cc_results VALUES (
      format('rows=%s groups=%s per_grp=%s count %s %s  [on only]',
             _n_rows, _n_groups, _n_rows / _n_groups, _op, _c),
      _n_rows, _n_groups, _n_rows / _n_groups, _op, _c,
      NULL, m_on, NULL, NULL);
  END IF;
END
$$ LANGUAGE plpgsql;


-- ----------------------------------------------------------------------
-- Bench grid.  Per-group N matters more than total rows : the off-path
-- DNF has k * binom(N, k) clauses, so we keep N <= 25 for the
-- off/on comparison shapes and reserve N >= 40 for on-only.
-- ----------------------------------------------------------------------
\set off_timeout '10s'

-- Small per-group (N = 10) : both paths fast, parity check.
SELECT cc_setup(40, 4);
SET statement_timeout = :'off_timeout'; SELECT cc_off(40, 4, '>=', 2); SET statement_timeout = 0; SELECT cc_on(40, 4, '>=', 2);
SET statement_timeout = :'off_timeout'; SELECT cc_off(40, 4, '>=', 5); SET statement_timeout = 0; SELECT cc_on(40, 4, '>=', 5);
SET statement_timeout = :'off_timeout'; SELECT cc_off(40, 4, '=',  5); SET statement_timeout = 0; SELECT cc_on(40, 4, '=',  5);

-- Medium per-group (N = 15) : binom(15, 7) = 6435, k*binom ~ 50k for
-- ">=" and exhaustive for "="; both manageable.
SELECT cc_setup(60, 4);
SET statement_timeout = :'off_timeout'; SELECT cc_off(60, 4, '>=', 2);  SET statement_timeout = 0; SELECT cc_on(60, 4, '>=', 2);
SET statement_timeout = :'off_timeout'; SELECT cc_off(60, 4, '>=', 7);  SET statement_timeout = 0; SELECT cc_on(60, 4, '>=', 7);
SET statement_timeout = :'off_timeout'; SELECT cc_off(60, 4, '=',  7);  SET statement_timeout = 0; SELECT cc_on(60, 4, '=',  7);
SET statement_timeout = :'off_timeout'; SELECT cc_off(60, 4, '<=', 7);  SET statement_timeout = 0; SELECT cc_on(60, 4, '<=', 7);

-- Larger per-group (N = 20) : binom(20, 10) = 184756.  ">= 10" is
-- ~1.85M DNF clauses (absorptive minimal generators) ; "= 10" is
-- the same count but with full missing factors (much heavier) so
-- it is intentionally OUT of the bench grid here to avoid OOM,
-- demonstrating one consequence of the absorptive-vs-non gap.
SELECT cc_setup(80, 4);
SET statement_timeout = :'off_timeout'; SELECT cc_off(80, 4, '>=', 2);  SET statement_timeout = 0; SELECT cc_on(80, 4, '>=', 2);
SET statement_timeout = :'off_timeout'; SELECT cc_off(80, 4, '>=', 5);  SET statement_timeout = 0; SELECT cc_on(80, 4, '>=', 5);
SET statement_timeout = :'off_timeout'; SELECT cc_off(80, 4, '>=', 10); SET statement_timeout = 0; SELECT cc_on(80, 4, '>=', 10);
SET statement_timeout = :'off_timeout'; SELECT cc_off(80, 4, '<=', 10); SET statement_timeout = 0; SELECT cc_on(80, 4, '<=', 10);

-- On-only shapes : per-group N too large for the off-path to fit
-- in memory (the absorptive DNF alone at N=25, k=12 is ~5M clauses
-- times 12 wires each ; "=" at the same point OOMs).
SELECT cc_setup(100, 4);  SELECT cc_on(100, 4, '>=', 12);
SELECT cc_setup(100, 4);  SELECT cc_on(100, 4, '=',  12);
SELECT cc_setup(160, 4);  SELECT cc_on(160, 4, '>=', 20);
SELECT cc_setup(400, 4);  SELECT cc_on(400, 4, '>=', 50);
SELECT cc_setup(1000, 10); SELECT cc_on(1000, 10, '>=', 50);

\echo
\echo '----------------------------------------------------------------------'
\echo 'HAVING COUNT op c : provsql.cmp_probability_evaluation off vs on'
\echo 'off-path capped at 10s ; TIMEOUT = pre-rewrite path too slow'
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
FROM cc_results ORDER BY n_rows, n_groups, op, c_thr;

DROP TABLE cc_t CASCADE;
DROP FUNCTION cc_setup(int, int);
DROP FUNCTION cc_off(int, int, text, int);
DROP FUNCTION cc_on(int, int, text, int);
