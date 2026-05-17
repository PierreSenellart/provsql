-- ----------------------------------------------------------------------
-- test/bench/absorption_bench.sql
--
-- Benchmark for the Boolean-only absorption rule (B3) in
-- foldBooleanIdentities, on a real-world-shaped UNION whose first
-- branch dominates pairs from the second branch :
--
--   SELECT city FROM employees
--   UNION
--   SELECT p1.city FROM employees p1, employees p2
--    WHERE p1.city = p2.city AND p1.name < p2.name
--
-- Branch 1 emits one provenance leaf per row.  Branch 2 emits a
-- gate_times(p1, p2) per ordered city-mate pair.  For each city the
-- UNION's plus root sees both : every gate_times(p1, p2) is
-- dominated by p1 (and p2) appearing as a branch-1 sibling, so
-- absorption collapses every pair, leaving a per-city OR over only
-- the branch-1 leaves.
--
-- For each (n_rows, n_cities) shape we compare :
--   * default chain with boolean_provenance = off
--     (independent throws, falls through to tree-decomposition /
--     external compiler on the full circuit)
--   * default chain with boolean_provenance = on
--     (foldBooleanIdentities absorbs the times-pairs, the circuit
--     becomes read-once, independent succeeds in linear time)
--
-- Run :
--   createdb absbench && psql absbench -X -f test/bench/absorption_bench.sql
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;
SELECT setseed(0.42);

DROP TABLE IF EXISTS ab_employees CASCADE;
DROP TABLE IF EXISTS ab_results CASCADE;
DROP TABLE IF EXISTS ab_baseline CASCADE;

CREATE TEMP TABLE ab_results(
  shape         text,
  n_rows        int,
  n_cities      int,
  pairs_per_city int,
  p_off_ms      numeric,
  p_on_ms       numeric,
  speedup       numeric,
  sanity_p_diff numeric
);

-- ----------------------------------------------------------------------
-- bench_one : build a population of n_rows employees distributed
-- across n_cities, run the dominant-pair UNION at both GUC settings,
-- record wall-clock + a sanity-check on per-city probabilities.
-- ----------------------------------------------------------------------
-- Set up a fresh ab_employees table at the requested size.
CREATE OR REPLACE FUNCTION ab_setup(_n_rows int, _n_cities int)
  RETURNS void AS $$
BEGIN
  DROP TABLE IF EXISTS ab_employees CASCADE;
  CREATE TABLE ab_employees(id int, name text, city text);
  INSERT INTO ab_employees
    SELECT i, 'emp_' || i, 'city_' || (i % _n_cities)
    FROM generate_series(1, _n_rows) i;
  PERFORM add_provenance('ab_employees');
  PERFORM set_prob(provsql, 0.3 + 0.6 * random()) FROM ab_employees;
END
$$ LANGUAGE plpgsql;

-- bench_off / bench_on run the off- and on-path respectively on the
-- already-populated ab_employees.  Each is called as a SEPARATE
-- top-level statement so the SESSION's statement_timeout arms a
-- fresh per-statement timer ; SET LOCAL inside a single big
-- plpgsql call would only change the GUC value without re-arming.
CREATE OR REPLACE FUNCTION bench_off(_n_rows int, _n_cities int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_off numeric;
  sum_off float;
  shape text;
  pairs_per_city int;
  timed_out boolean := false;
BEGIN
  pairs_per_city := (_n_rows / _n_cities) * (_n_rows / _n_cities - 1) / 2;
  shape := format('rows=%s, cities=%s (~%s pairs/city)',
                  _n_rows, _n_cities, pairs_per_city);

  SET LOCAL provsql.boolean_provenance = off;
  PERFORM count(*) FROM (
    SELECT city FROM ab_employees
    UNION
    SELECT p1.city FROM ab_employees p1, ab_employees p2
     WHERE p1.city = p2.city AND p1.name < p2.name
  ) t;
  t0 := clock_timestamp();
  BEGIN
    CREATE TEMP TABLE ab_baseline AS
      SELECT city, probability_evaluate(provenance()) AS p
        FROM (
          SELECT city FROM ab_employees
          UNION
          SELECT p1.city FROM ab_employees p1, ab_employees p2
           WHERE p1.city = p2.city AND p1.name < p2.name
        ) t;
  EXCEPTION WHEN query_canceled THEN timed_out := true;
  END;
  t1 := clock_timestamp();
  m_off := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);
  IF timed_out THEN
    sum_off := 0;
  ELSE
    SELECT coalesce(sum(p), 0) FROM ab_baseline INTO sum_off;
    PERFORM remove_provenance('ab_baseline');
    DROP TABLE ab_baseline;
  END IF;

  -- Stash partial off-path measurement ; bench_on populates the
  -- matching on-path columns and computes speedup/diff.
  INSERT INTO ab_results VALUES (
    shape, _n_rows, _n_cities, pairs_per_city,
    m_off, NULL,
    NULL,
    CASE WHEN timed_out THEN NULL ELSE sum_off END
  );
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION bench_on(_n_rows int, _n_cities int)
  RETURNS void AS $$
DECLARE
  t0 timestamptz; t1 timestamptz;
  m_on  numeric;
  sum_on float;
  pairs_per_city int;
  off_sum float;
  off_ms numeric;
  diff numeric;
BEGIN
  pairs_per_city := (_n_rows / _n_cities) * (_n_rows / _n_cities - 1) / 2;

  SET LOCAL provsql.boolean_provenance = on;
  PERFORM count(*) FROM (
    SELECT city FROM ab_employees
    UNION
    SELECT p1.city FROM ab_employees p1, ab_employees p2
     WHERE p1.city = p2.city AND p1.name < p2.name
  ) t;
  t0 := clock_timestamp();
  CREATE TEMP TABLE ab_rewritten AS
    SELECT city, probability_evaluate(provenance()) AS p
      FROM (
        SELECT city FROM ab_employees
        UNION
        SELECT p1.city FROM ab_employees p1, ab_employees p2
         WHERE p1.city = p2.city AND p1.name < p2.name
      ) t;
  t1 := clock_timestamp();
  m_on := round((EXTRACT(EPOCH FROM (t1 - t0)) * 1000)::numeric, 3);
  SELECT coalesce(sum(p), 0) FROM ab_rewritten INTO sum_on;
  PERFORM remove_provenance('ab_rewritten');
  DROP TABLE ab_rewritten;

  -- Fold the on-path columns into the matching off-path row stashed
  -- by bench_off ; if no off row exists (on-only call) insert fresh.
  SELECT p_off_ms, sanity_p_diff
    FROM ab_results
    WHERE n_rows = _n_rows AND n_cities = _n_cities
      AND p_on_ms IS NULL
    LIMIT 1
    INTO off_ms, off_sum;
  IF off_ms IS NOT NULL THEN
    diff := CASE WHEN off_sum IS NULL THEN NULL
                 ELSE round(abs(off_sum - sum_on)::numeric, 6) END;
    UPDATE ab_results
      SET p_on_ms = m_on,
          speedup = CASE WHEN off_sum IS NULL THEN NULL
                         ELSE round((off_ms / NULLIF(m_on, 0))::numeric, 2)
                    END,
          sanity_p_diff = diff
      WHERE n_rows = _n_rows AND n_cities = _n_cities AND p_on_ms IS NULL;
  ELSE
    INSERT INTO ab_results VALUES (
      format('rows=%s, cities=%s (~%s pairs/city)  [on only]',
             _n_rows, _n_cities, pairs_per_city),
      _n_rows, _n_cities, pairs_per_city,
      NULL, m_on, NULL, NULL
    );
  END IF;
END
$$ LANGUAGE plpgsql;


-- ----------------------------------------------------------------------
-- Bench grid.  Each row scales the population ; pairs/city is the
-- O(rows/cities)^2 quantity that drives the off-path's pain.
-- ----------------------------------------------------------------------
-- Drive : per shape, set up data, then run off-path under a 10s
-- timer (top-level SET arms the per-statement timer), then on-path
-- without timer.  Off-path TIMEOUT rows are recorded by bench_off
-- as having NULL sum_diff which the report formats specially.
\set off_timeout '10s'

SELECT ab_setup(20,   4);   SET statement_timeout = :'off_timeout'; SELECT bench_off(20,   4); SET statement_timeout = 0; SELECT bench_on(20,   4);
SELECT ab_setup(40,   4);   SET statement_timeout = :'off_timeout'; SELECT bench_off(40,   4); SET statement_timeout = 0; SELECT bench_on(40,   4);
SELECT ab_setup(80,   4);   SET statement_timeout = :'off_timeout'; SELECT bench_off(80,   4); SET statement_timeout = 0; SELECT bench_on(80,   4);
SELECT ab_setup(160,  4);   SET statement_timeout = :'off_timeout'; SELECT bench_off(160,  4); SET statement_timeout = 0; SELECT bench_on(160,  4);
SELECT ab_setup(80,   8);   SET statement_timeout = :'off_timeout'; SELECT bench_off(80,   8); SET statement_timeout = 0; SELECT bench_on(80,   8);
SELECT ab_setup(160,  8);   SET statement_timeout = :'off_timeout'; SELECT bench_off(160,  8); SET statement_timeout = 0; SELECT bench_on(160,  8);
SELECT ab_setup(320,  8);   SET statement_timeout = :'off_timeout'; SELECT bench_off(320,  8); SET statement_timeout = 0; SELECT bench_on(320,  8);

-- On-only shapes : skip the off-path entirely.
SELECT ab_setup(320,  4); SELECT bench_on(320,  4);
SELECT ab_setup(640,  4); SELECT bench_on(640,  4);
SELECT ab_setup(640,  8); SELECT bench_on(640,  8);
SELECT ab_setup(1280, 8); SELECT bench_on(1280, 8);

\echo
\echo '----------------------------------------------------------------------'
\echo 'Absorption-driven UNION : boolean_provenance off vs on'
\echo 'off-path capped at 10s ; TIMEOUT = full circuit too slow'
\echo '[on only] rows : the off path was skipped as it is intractable'
\echo '----------------------------------------------------------------------'
SELECT shape,
       CASE WHEN p_off_ms IS NULL THEN '(skipped)'
            WHEN sanity_p_diff IS NULL THEN 'TIMEOUT (>'
                                         || p_off_ms::text || ' ms)'
            ELSE p_off_ms::text || ' ms' END  AS off_time,
       p_on_ms   || ' ms' AS on_time,
       CASE WHEN p_off_ms IS NULL THEN '-'
            WHEN speedup IS NULL THEN 'inf'
            ELSE speedup::text || 'x' END     AS speedup,
       sanity_p_diff AS abs_sum_diff
FROM ab_results ORDER BY n_rows, n_cities, shape;

DROP TABLE ab_employees CASCADE;
DROP FUNCTION ab_setup(int, int);
DROP FUNCTION bench_off(int, int);
DROP FUNCTION bench_on(int, int);
