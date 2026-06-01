-- ----------------------------------------------------------------------
-- test/bench/having_safe_join_count_bench.sql
--
-- Benchmark for the safe-join COUNT marginal-vector pre-pass
-- (src/AggMarginalEvaluator.cpp) on the canonical single-level fan-out
-- HAVING-COUNT query shape :
--
--   SELECT k, probability_evaluate(provenance())
--   FROM sj_r JOIN sj_s ON sj_r.a = sj_s.a
--   GROUP BY k HAVING count(*) <op> c
--
-- sj_r holds one row (k, k) per group, sj_s holds <per_grp> rows
-- (k, b) per group ; joining on a = k gives each group <per_grp> join
-- tuples that all share the single sj_r(k, k) leaf -- a fan-out block.
-- Because the contributors share a leaf they are NOT independent, so
-- the flat Poisson-binomial pre-pass (runCountCmpEvaluator) bails and
-- the off-path falls through to provsql_having's enumerate_valid_worlds,
-- which iterates ALL 2^(per_grp+1) possible worlds of a group.  The
-- marginal-vector engine replaces that with the per-block mixture +
-- O(per_grp^2) convolution.
--
-- per_grp is the cost driver : the off-path enumerates 2^per_grp worlds,
-- so the off/on comparison shapes keep per_grp <= 20 (2^20 ~ 1M) and the
-- on-only shapes reach per_grp >= 30 where the off-path is hopeless.
-- Each off-path shape runs under a 10s statement_timeout ; off-path
-- TIMEOUT rows are recorded with a NULL p_off_ms so the report flags them.
--
-- Run :
--   createdb sjbench && psql sjbench -X -f test/bench/having_safe_join_count_bench.sql
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;
SELECT setseed(0.42);

DROP TABLE IF EXISTS sj_results CASCADE;
CREATE TEMP TABLE sj_results(
  shape     text,
  n_groups  int,
  per_grp   int,
  op        text,
  c_thr     int,
  p_off_ms  numeric,
  p_on_ms   numeric,
  speedup   numeric,
  diff_max  numeric
);

-- Build sj_r / sj_s at the requested per-group fan-out, Bernoulli
-- probabilities in [0.3, 0.9].
CREATE OR REPLACE FUNCTION sj_setup(_n_groups int, _per_grp int)
  RETURNS void AS $$
BEGIN
  DROP TABLE IF EXISTS sj_r CASCADE;
  DROP TABLE IF EXISTS sj_s CASCADE;
  CREATE TABLE sj_r(k int, a int);
  CREATE TABLE sj_s(a int, b int);
  INSERT INTO sj_r SELECT g, g FROM generate_series(1, _n_groups) g;
  INSERT INTO sj_s
    SELECT g, b FROM generate_series(1, _n_groups) g,
                     generate_series(1, _per_grp) b;
  PERFORM add_provenance('sj_r');
  PERFORM add_provenance('sj_s');
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM sj_r;
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM sj_s;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION sj_off(_n_groups int, _per_grp int,
                                  _op text, _c int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_off numeric;
  shape text;
  q text;
  timed_out boolean := false;
BEGIN
  shape := format('groups=%s per_grp=%s count(*) %s %s',
                  _n_groups, _per_grp, _op, _c);
  SET LOCAL provsql.cmp_probability_evaluation = off;
  q := format(
    'SELECT k, probability_evaluate(provenance()) AS p '
    'FROM sj_r JOIN sj_s ON sj_r.a = sj_s.a '
    'GROUP BY k HAVING count(*) %s %s', _op, _c);
  t0 := clock_timestamp();
  BEGIN
    EXECUTE 'CREATE TEMP TABLE sj_offr AS ' || q;
  EXCEPTION WHEN query_canceled THEN timed_out := true;
  END;
  t1 := clock_timestamp();
  m_off := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);
  IF NOT timed_out THEN PERFORM remove_provenance('sj_offr'); END IF;
  INSERT INTO sj_results VALUES (
    shape, _n_groups, _per_grp, _op, _c,
    CASE WHEN timed_out THEN NULL ELSE m_off END, NULL, NULL, NULL);
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION sj_on(_n_groups int, _per_grp int,
                                 _op text, _c int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_on numeric;
  q text;
  off_ms numeric;
  off_exists boolean;
  max_diff numeric;
  pending boolean;
BEGIN
  SET LOCAL provsql.cmp_probability_evaluation = on;
  q := format(
    'SELECT k, probability_evaluate(provenance()) AS p '
    'FROM sj_r JOIN sj_s ON sj_r.a = sj_s.a '
    'GROUP BY k HAVING count(*) %s %s', _op, _c);
  t0 := clock_timestamp();
  EXECUTE 'CREATE TEMP TABLE sj_onr AS ' || q;
  t1 := clock_timestamp();
  m_on := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);

  SELECT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'sj_offr')
    INTO off_exists;
  IF off_exists THEN
    EXECUTE 'SELECT coalesce(max(ABS(o.p - n.p)), 0) '
            'FROM sj_offr o JOIN sj_onr n USING (k)' INTO max_diff;
  ELSE
    max_diff := NULL;
  END IF;

  PERFORM remove_provenance('sj_onr');
  EXECUTE 'DROP TABLE sj_onr';
  IF off_exists THEN EXECUTE 'DROP TABLE sj_offr'; END IF;

  SELECT EXISTS (SELECT 1 FROM sj_results
                 WHERE n_groups = _n_groups AND per_grp = _per_grp
                   AND op = _op AND c_thr = _c AND p_on_ms IS NULL)
    INTO pending;

  IF pending THEN
    SELECT p_off_ms FROM sj_results
      WHERE n_groups = _n_groups AND per_grp = _per_grp
        AND op = _op AND c_thr = _c AND p_on_ms IS NULL
      LIMIT 1 INTO off_ms;
    UPDATE sj_results
      SET p_on_ms = m_on,
          speedup = CASE WHEN off_ms IS NULL OR max_diff IS NULL THEN NULL
                         ELSE round((off_ms / NULLIF(m_on, 0))::numeric, 1) END,
          diff_max = CASE WHEN max_diff IS NULL THEN NULL
                          ELSE round(max_diff, 6) END
      WHERE n_groups = _n_groups AND per_grp = _per_grp
        AND op = _op AND c_thr = _c AND p_on_ms IS NULL;
  ELSE
    INSERT INTO sj_results VALUES (
      format('groups=%s per_grp=%s count(*) %s %s  [on only]',
             _n_groups, _per_grp, _op, _c),
      _n_groups, _per_grp, _op, _c, NULL, m_on, NULL, NULL);
  END IF;
END
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- Bench grid.  per_grp drives the 2^per_grp off-path ; n_groups = 3.
-- ----------------------------------------------------------------------
\set off_timeout '10s'

-- per_grp = 12 : 2^12 = 4096 worlds/group, both paths fast (parity).
SELECT sj_setup(3, 12);
SET statement_timeout = :'off_timeout'; SELECT sj_off(3, 12, '>=', 6); SET statement_timeout = 0; SELECT sj_on(3, 12, '>=', 6);

-- per_grp = 15 : 2^15 = 32768 worlds/group.
SELECT sj_setup(3, 15);
SET statement_timeout = :'off_timeout'; SELECT sj_off(3, 15, '>=', 8); SET statement_timeout = 0; SELECT sj_on(3, 15, '>=', 8);
SET statement_timeout = :'off_timeout'; SELECT sj_off(3, 15, '=',  8); SET statement_timeout = 0; SELECT sj_on(3, 15, '=',  8);

-- per_grp = 18 : 2^18 = 262144 worlds/group.
SELECT sj_setup(3, 18);
SET statement_timeout = :'off_timeout'; SELECT sj_off(3, 18, '>=', 9); SET statement_timeout = 0; SELECT sj_on(3, 18, '>=', 9);

-- per_grp = 20 : 2^20 ~ 1.05M worlds/group ; at or past the 10s edge.
SELECT sj_setup(3, 20);
SET statement_timeout = :'off_timeout'; SELECT sj_off(3, 20, '>=', 10); SET statement_timeout = 0; SELECT sj_on(3, 20, '>=', 10);

-- On-only shapes : per_grp far past the 2^N off-path's reach.
SELECT sj_setup(3, 30);  SELECT sj_on(3, 30, '>=', 15);
SELECT sj_setup(3, 50);  SELECT sj_on(3, 50, '>=', 25);
SELECT sj_setup(3, 100); SELECT sj_on(3, 100, '>=', 50);

\pset format aligned
\echo ''
\echo '==================== safe-join COUNT benchmark ===================='
SELECT shape,
       CASE WHEN shape LIKE '%[on only]%' THEN 'n/a (2^N)'
            WHEN p_off_ms IS NULL          THEN 'TIMEOUT'
            ELSE p_off_ms::text END        AS off_ms,
       p_on_ms                             AS on_ms,
       coalesce(speedup::text, '-')        AS speedup,
       coalesce(diff_max::text, '-')       AS diff_max
FROM sj_results
ORDER BY per_grp, op, c_thr;

DROP TABLE IF EXISTS sj_r CASCADE;
DROP TABLE IF EXISTS sj_s CASCADE;
