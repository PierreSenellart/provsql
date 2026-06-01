-- ----------------------------------------------------------------------
-- test/bench/min_max_cmp_bench.sql
--
-- Benchmark for the MIN / MAX closed-form pre-pass
-- (provsql.cmp_probability_evaluation) on the canonical flat-table
-- HAVING-MIN / MAX query shape :
--
--   SELECT g, probability_evaluate(provenance())
--   FROM mm_t GROUP BY g HAVING <agg>(v) <op> c
--
-- where mm_t has n_rows tuples evenly across n_groups groups, each row
-- carrying a per-row value v.  Unlike COUNT (whose off-path is the
-- binom(N, k) minimal-generator DNF), MIN / MAX fall through to
-- enumerate_exhaustive, which iterates ALL 2^N possible worlds of a
-- per-group of size N -- so the off-path blows up in N itself, not in
-- the threshold.  The closed form replaces all of that with a single
-- O(N) product over the children partitioned on v vs c.
--
-- The off/on comparison shapes therefore keep per-group N <= 20
-- (2^20 ~ 1M worlds) to stay inside memory ; on-only shapes reach
-- N >= 30 where the off-path is hopeless.  Each off-path shape runs
-- under a 10s statement_timeout (top-level SET arms the per-statement
-- timer) ; off-path TIMEOUT rows are recorded with a (skipped)
-- diff_max so the report flags them.
--
-- Run :
--   createdb mmbench && psql mmbench -X -f test/bench/min_max_cmp_bench.sql
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;
SELECT setseed(0.42);

DROP TABLE IF EXISTS mm_t CASCADE;
DROP TABLE IF EXISTS mm_results CASCADE;

CREATE TEMP TABLE mm_results(
  shape    text,
  n_rows   int,
  n_groups int,
  per_grp  int,
  agg      text,
  op       text,
  c_thr    int,
  p_off_ms numeric,
  p_on_ms  numeric,
  speedup  numeric,
  diff_max numeric
);

-- Set up a fresh mm_t at the requested size, with per-row value v in
-- [1, 100] and per-row Bernoulli probabilities in [0.3, 0.9].
CREATE OR REPLACE FUNCTION mm_setup(_n_rows int, _n_groups int)
  RETURNS void AS $$
BEGIN
  DROP TABLE IF EXISTS mm_t CASCADE;
  CREATE TABLE mm_t(id int, g text, v int);
  INSERT INTO mm_t
    SELECT i, 'g_' || (i % _n_groups), 1 + (random() * 99)::int
    FROM generate_series(1, _n_rows) i;
  PERFORM add_provenance('mm_t');
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM mm_t;
END
$$ LANGUAGE plpgsql;

-- mm_off / mm_on run the off- and on-path on the already-populated
-- mm_t for a given aggregate + operator + threshold.  Called as
-- SEPARATE top-level statements so SESSION statement_timeout arms a
-- fresh per-statement timer.
CREATE OR REPLACE FUNCTION mm_off(_n_rows int, _n_groups int,
                                   _agg text, _op text, _c int)
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
  shape := format('rows=%s groups=%s per_grp=%s %s(v) %s %s',
                  _n_rows, _n_groups, per_grp, _agg, _op, _c);

  SET LOCAL provsql.cmp_probability_evaluation = off;
  q := format(
    'SELECT g, probability_evaluate(provenance()) AS p '
    'FROM mm_t GROUP BY g HAVING %s(v) %s %s', _agg, _op, _c);

  t0 := clock_timestamp();
  BEGIN
    EXECUTE 'CREATE TEMP TABLE mm_off AS ' || q;
  EXCEPTION WHEN query_canceled THEN timed_out := true;
  END;
  t1 := clock_timestamp();
  m_off := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  IF NOT timed_out THEN
    PERFORM remove_provenance('mm_off');
  END IF;

  INSERT INTO mm_results VALUES (
    shape, _n_rows, _n_groups, per_grp, _agg, _op, _c,
    m_off, NULL, NULL, NULL);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION mm_on(_n_rows int, _n_groups int,
                                  _agg text, _op text, _c int)
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
    'FROM mm_t GROUP BY g HAVING %s(v) %s %s', _agg, _op, _c);

  t0 := clock_timestamp();
  EXECUTE 'CREATE TEMP TABLE mm_on AS ' || q;
  t1 := clock_timestamp();
  m_on := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'mm_off')
    INTO off_exists;
  IF off_exists THEN
    EXECUTE 'SELECT coalesce(max(ABS(o.p - n.p)), 0) '
            'FROM mm_off o JOIN mm_on n USING (g)' INTO max_diff;
  ELSE
    max_diff := NULL;
  END IF;

  PERFORM remove_provenance('mm_on');
  EXECUTE 'DROP TABLE mm_on';
  IF off_exists THEN EXECUTE 'DROP TABLE mm_off'; END IF;

  SELECT p_off_ms FROM mm_results
    WHERE n_rows = _n_rows AND n_groups = _n_groups
      AND agg = _agg AND op = _op AND c_thr = _c AND p_on_ms IS NULL
    LIMIT 1 INTO off_ms;
  IF off_ms IS NOT NULL THEN
    UPDATE mm_results
      SET p_on_ms = m_on,
          speedup = CASE WHEN max_diff IS NULL THEN NULL
                         ELSE round((off_ms / NULLIF(m_on, 0))::numeric, 2) END,
          diff_max = round(coalesce(max_diff, 0), 6)
      WHERE n_rows = _n_rows AND n_groups = _n_groups
        AND agg = _agg AND op = _op AND c_thr = _c AND p_on_ms IS NULL;
  ELSE
    INSERT INTO mm_results VALUES (
      format('rows=%s groups=%s per_grp=%s %s(v) %s %s  [on only]',
             _n_rows, _n_groups, _n_rows / _n_groups, _agg, _op, _c),
      _n_rows, _n_groups, _n_rows / _n_groups, _agg, _op, _c,
      NULL, m_on, NULL, NULL);
  END IF;
END
$$ LANGUAGE plpgsql;


-- ----------------------------------------------------------------------
-- Bench grid.  Per-group N is the cost driver : the off-path enumerates
-- 2^N worlds, so the off/on comparison shapes keep N <= 20 and the
-- on-only shapes reserve N >= 30.
-- ----------------------------------------------------------------------
\set off_timeout '10s'

-- Small per-group (N = 10) : both paths fast, parity check.
SELECT mm_setup(40, 4);
SET statement_timeout = :'off_timeout'; SELECT mm_off(40, 4, 'max', '>=', 50); SET statement_timeout = 0; SELECT mm_on(40, 4, 'max', '>=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(40, 4, 'min', '<=', 50); SET statement_timeout = 0; SELECT mm_on(40, 4, 'min', '<=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(40, 4, 'max', '=',  50); SET statement_timeout = 0; SELECT mm_on(40, 4, 'max', '=',  50);

-- Medium per-group (N = 15) : 2^15 = 32768 worlds.
SELECT mm_setup(60, 4);
SET statement_timeout = :'off_timeout'; SELECT mm_off(60, 4, 'max', '>=', 50); SET statement_timeout = 0; SELECT mm_on(60, 4, 'max', '>=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(60, 4, 'min', '>=', 50); SET statement_timeout = 0; SELECT mm_on(60, 4, 'min', '>=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(60, 4, 'max', '<>', 50); SET statement_timeout = 0; SELECT mm_on(60, 4, 'max', '<>', 50);

-- Larger per-group (N = 18) : 2^18 = 262144 worlds.
SELECT mm_setup(72, 4);
SET statement_timeout = :'off_timeout'; SELECT mm_off(72, 4, 'max', '>=', 50); SET statement_timeout = 0; SELECT mm_on(72, 4, 'max', '>=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(72, 4, 'min', '<=', 50); SET statement_timeout = 0; SELECT mm_on(72, 4, 'min', '<=', 50);

-- Threshold of blow-up (N = 20) : 2^20 ~ 1.05M worlds ; the off-path
-- materialises a DNF that large and is at or past the 10s edge.
SELECT mm_setup(80, 4);
SET statement_timeout = :'off_timeout'; SELECT mm_off(80, 4, 'max', '>=', 50); SET statement_timeout = 0; SELECT mm_on(80, 4, 'max', '>=', 50);
SET statement_timeout = :'off_timeout'; SELECT mm_off(80, 4, 'min', '>=', 50); SET statement_timeout = 0; SELECT mm_on(80, 4, 'min', '>=', 50);

-- On-only shapes : per-group N too large for the 2^N off-path.
SELECT mm_setup(120,  4);  SELECT mm_on(120,  4, 'max', '>=', 50);
SELECT mm_setup(120,  4);  SELECT mm_on(120,  4, 'min', '=',  50);
SELECT mm_setup(400,  4);  SELECT mm_on(400,  4, 'max', '>=', 50);
SELECT mm_setup(1000, 10); SELECT mm_on(1000, 10, 'max', '>=', 50);

\echo
\echo '----------------------------------------------------------------------'
\echo 'HAVING <agg>(v) op c : provsql.cmp_probability_evaluation off vs on'
\echo 'off-path capped at 10s ; TIMEOUT = exhaustive 2^N path too slow'
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
FROM mm_results ORDER BY n_rows, n_groups, agg, op, c_thr;

DROP TABLE mm_t CASCADE;
DROP FUNCTION mm_setup(int, int);
DROP FUNCTION mm_off(int, int, text, text, int);
DROP FUNCTION mm_on(int, int, text, text, int);
