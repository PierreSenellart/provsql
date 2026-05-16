-- ----------------------------------------------------------------------
-- test/bench/shapley_bench.sql
--
-- Benchmark for the impact of the safe-query (Boolean-rewrite)
-- optimisation on Shapley-value computation.
--
-- Shapley evaluation in ProvSQL goes through `BooleanCircuit::makeDD`
-- (src/BooleanCircuit.cpp), which first attempts `interpretAsDD()`
-- (structural d-DNNF check), then falls back to a tree decomposition,
-- and finally to external d-DNNF compilation (`d4` by default). The
-- safe-query rewrite (`provsql.boolean_provenance = ON`) produces a
-- read-once / structurally-d-DNNF circuit for hierarchical CQs, so
-- `interpretAsDD()` succeeds directly and `makeDD` returns without
-- ever invoking tree decomposition or an external compiler. That is
-- the speedup this benchmark measures.
--
-- For each query shape it runs the same hierarchical CQ twice:
--
--   * provsql.boolean_provenance = OFF, shapley_all_vars(prov)
--     (the unrewritten circuit forces makeDD into tree decomposition
--     or external d-DNNF compilation)
--   * provsql.boolean_provenance = ON, shapley_all_vars(prov)
--     (the rewritten read-once circuit is interpreted as d-DNNF
--     directly, with no compiler call)
--
-- and reports for each:
--
--   * output cardinality under each setting (must match)
--   * sum of per-row Shapley-sums under each setting (by the
--     efficiency property of Shapley, the per-row sum across all
--     variables equals the probability of that row evaluating to
--     true, so the two settings must agree modulo FP rounding and
--     also agree with the probability_evaluate baseline)
--   * wall-clock time of each evaluation
--   * speedup ratio (off_time / on_time)
--
-- Run from psql against a fresh or existing database:
--   createdb bench && psql bench -X -f test/bench/shapley_bench.sql
--
-- The server must have `shared_preload_libraries='provsql'` set; the
-- script installs the extension itself if necessary. Tables and
-- helper objects use the `bench_*` prefix and are dropped at the end.
--
-- The default method (auto-detect) is used for both runs: that is
-- the realistic case and the one in which the rewrite makes the
-- difference. Forcing `'tree-decomposition'` or `'compilation'`
-- would defeat the comparison by short-circuiting the dispatcher.
--
-- BID shapes are excluded: Shapley rejects circuits with mulinput
-- gates (src/shapley.cpp), so `bench_bid` from the safe-query bench
-- has no analogue here.
-- ----------------------------------------------------------------------

\set ECHO none
\pset format aligned
\pset pager off

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO public, provsql;

-- Deterministic RNG so successive runs are comparable.
SELECT setseed(0.42);

-- ----------------------------------------------------------------------
-- Setup: scratch tables and helper to bench one query shape.
-- ----------------------------------------------------------------------

DROP TABLE IF EXISTS bench_a, bench_b, bench_c, bench_d, bench_e CASCADE;

-- Sizes are tighter than the safe-query bench because Shapley adds
-- per-variable d-DNNF walks on top of makeDD (makeSmooth +
-- makeGatesBinary + an O(|inputs|) traversal), so the OFF path
-- scales worse than probability_evaluate on the same circuit.
-- Bump only if you have time; the OFF path can blow up
-- super-linearly for non-trivial shapes.
-- n_x       : distinct root-variable values
-- n_y_per_x : distinct y per x
-- n_z_per_y : distinct z per (x, y)
-- n_w_per_x : distinct w per x
\set n_x       4
\set n_y_per_x 3
\set n_z_per_y 2
\set n_w_per_x 2

-- Per-evaluation wall-clock cap. The rewriter (ON) is linear in
-- circuit size, so even the most demanding shape should finish in
-- milliseconds; the OFF path may bail with a statement-timeout
-- error and bench_one will record that in the report.
SET statement_timeout = '60s';

-- bench_a(x, y, z): full (x, y, z) product, size n_x*n_y_per_x*n_z_per_y
CREATE TABLE bench_a AS
  SELECT x, y, z
    FROM generate_series(1, :n_x)        AS x,
         generate_series(1, :n_y_per_x)  AS y,
         generate_series(1, :n_z_per_y)  AS z;

-- bench_b(x, y, z): same shape as A, independent rows.
CREATE TABLE bench_b AS SELECT * FROM bench_a;

-- bench_c(x, y): (x, y) projection.
CREATE TABLE bench_c AS SELECT DISTINCT x, y FROM bench_a;

-- bench_d(x, w): (x, w) carrier.
CREATE TABLE bench_d AS
  SELECT x, w
    FROM generate_series(1, :n_x)       AS x,
         generate_series(1, :n_w_per_x) AS w;

-- bench_e(x, w): mirror of D.
CREATE TABLE bench_e AS SELECT * FROM bench_d;

\echo 'Building provenance and setting random probabilities...'
SELECT add_provenance('bench_a');
SELECT add_provenance('bench_b');
SELECT add_provenance('bench_c');
SELECT add_provenance('bench_d');
SELECT add_provenance('bench_e');

DO $$ BEGIN
  PERFORM set_prob(provsql, random()) FROM bench_a;
  PERFORM set_prob(provsql, random()) FROM bench_b;
  PERFORM set_prob(provsql, random()) FROM bench_c;
  PERFORM set_prob(provsql, random()) FROM bench_d;
  PERFORM set_prob(provsql, random()) FROM bench_e;
END $$;

-- One row per benchmarked query: the OFF and ON output cardinalities
-- (must agree for a sound rewrite), the sum of per-row Shapley-sums
-- under each setting (efficiency property: per-row sum across all
-- variables equals the row's probability, so the two settings must
-- agree modulo FP precision), the absolute sum-diff, the wall-clock
-- time of each evaluation, and the speedup ratio.
CREATE TEMP TABLE bench_results (
  shape       text,
  rows_off    int,
  rows_on     int,
  sum_p_off   double precision,
  sum_p_on    double precision,
  abs_diff    double precision,
  off_secs    double precision,
  on_secs     double precision,
  speedup     double precision
) ;

-- Run @p qry under both modes; aggregate timings and store one row
-- in bench_results. Soundness is asserted via output cardinality
-- (rows_off == rows_on) and the sum of per-row Shapley-sums
-- (sum_p_off == sum_p_on modulo FP precision; by efficiency these
-- equal the row's probability, so they coincide with the
-- corresponding safe_query_bench numbers as well). When either side
-- hits @c statement_timeout (or raises any other exception),
-- @c bench_one records NULLs in the result columns it could not
-- compute and continues with the next shape rather than aborting
-- the whole run. The @c EXCEPTION clause names @c query_canceled
-- explicitly because @c WHEN OTHERS in plpgsql does not catch
-- @c query_canceled / @c assert_failure on its own.
CREATE OR REPLACE FUNCTION bench_one(shape text, qry text)
  RETURNS void AS $$
DECLARE
  t0           timestamptz;
  t1           timestamptz;
  off_secs     double precision;
  on_secs      double precision;
  rows_off     int          := NULL;
  rows_on      int          := NULL;
  sum_p_off    double precision := NULL;
  sum_p_on     double precision := NULL;
  off_ok       bool         := false;
  on_ok        bool         := false;
BEGIN
  -- OFF
  SET LOCAL provsql.boolean_provenance = off;
  EXECUTE format('CREATE TEMP TABLE bench_off  AS %s', qry);
  PERFORM remove_provenance('bench_off');
  BEGIN
    t0 := clock_timestamp();
    EXECUTE 'CREATE TEMP TABLE bench_off_sh AS
             SELECT (SELECT sum(value)
                       FROM shapley_all_vars(prov)) AS s
               FROM bench_off';
    t1 := clock_timestamp();
    off_secs := EXTRACT(EPOCH FROM (t1 - t0));
    off_ok := true;
  EXCEPTION WHEN query_canceled OR OTHERS THEN
    off_secs := EXTRACT(EPOCH FROM (clock_timestamp() - t0));
    RAISE NOTICE 'OFF % failed after %s: %', shape, off_secs, SQLERRM;
  END;

  -- ON
  SET LOCAL provsql.boolean_provenance = on;
  EXECUTE format('CREATE TEMP TABLE bench_on  AS %s', qry);
  PERFORM remove_provenance('bench_on');
  BEGIN
    t0 := clock_timestamp();
    EXECUTE 'CREATE TEMP TABLE bench_on_sh AS
             SELECT (SELECT sum(value)
                       FROM shapley_all_vars(prov)) AS s
               FROM bench_on';
    t1 := clock_timestamp();
    on_secs := EXTRACT(EPOCH FROM (t1 - t0));
    on_ok := true;
  EXCEPTION WHEN query_canceled OR OTHERS THEN
    on_secs := EXTRACT(EPOCH FROM (clock_timestamp() - t0));
    RAISE NOTICE 'ON % failed after %s: %', shape, on_secs, SQLERRM;
  END;

  IF off_ok THEN
    SELECT count(*), sum(s) INTO rows_off, sum_p_off FROM bench_off_sh;
  END IF;
  IF on_ok THEN
    SELECT count(*), sum(s) INTO rows_on, sum_p_on FROM bench_on_sh;
  END IF;

  INSERT INTO bench_results VALUES (
    shape, rows_off, rows_on, sum_p_off, sum_p_on,
    CASE WHEN off_ok AND on_ok THEN abs(sum_p_off - sum_p_on) END,
    off_secs, on_secs,
    CASE WHEN on_ok AND off_ok AND on_secs > 0
         THEN off_secs / on_secs ELSE NULL END);

  DROP TABLE bench_off;
  IF off_ok THEN DROP TABLE bench_off_sh; END IF;
  DROP TABLE bench_on;
  IF on_ok  THEN DROP TABLE bench_on_sh;  END IF;
END;
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- Shapes to benchmark. Each is a hierarchical CQ that the safe-query
-- rewriter handles end-to-end; under ON the resulting circuit is
-- structurally d-DNNF and Shapley compilation collapses to a
-- single interpretAsDD pass.
-- ----------------------------------------------------------------------

SELECT bench_one(
  '2-atom: A(x) ⋈ B(x), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b WHERE a.x = b.x GROUP BY a.x');

SELECT bench_one(
  '3-atom: A(x) ⋈ B(x) ⋈ C(x), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c
    WHERE a.x = b.x AND a.x = c.x GROUP BY a.x');

SELECT bench_one(
  'pushdown: A(x,y) ⋈ B(x,y) ⋈ C(x,y), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c
    WHERE a.x = b.x AND a.x = c.x AND a.y = b.y AND a.y = c.y
    GROUP BY a.x');

SELECT bench_one(
  'multi-level: A(x,y,z) ⋈ B(x,y,z) ⋈ C(x,y), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c
    WHERE a.x = b.x AND a.x = c.x AND a.y = b.y AND a.y = c.y
      AND a.z = b.z
    GROUP BY a.x');

SELECT bench_one(
  '2 components: (A ⋈ B on x,y,z) × (D ⋈ E on x,w), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_d d, bench_e e
    WHERE a.x = b.x AND a.x = d.x AND a.x = e.x
      AND a.y = b.y AND a.z = b.z AND d.w = e.w
    GROUP BY a.x');

SELECT bench_one(
  'bridge: 5 atoms, y bridges {A,B} and {C}, w in {D,E}',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c, bench_d d, bench_e e
    WHERE a.x = b.x AND a.x = c.x AND a.x = d.x AND a.x = e.x
      AND a.y = b.y AND a.y = c.y AND a.z = b.z AND d.w = e.w
    GROUP BY a.x');

-- (7) 4 atoms joined on x only: A, B, C, D, no other column shared
SELECT bench_one(
  '4-atom plain root: A,B,C,D on x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c, bench_d d
    WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
    GROUP BY a.x');

-- (8) 5 atoms joined on x only: stress on the gate_times width
SELECT bench_one(
  '5-atom plain root: A,B,C,D,E on x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c, bench_d d, bench_e e
    WHERE a.x = b.x AND a.x = c.x AND a.x = d.x AND a.x = e.x
    GROUP BY a.x');

-- (9) SELECT DISTINCT instead of GROUP BY: equivalent row-count
-- semantics; transform_distinct_into_group_by promotes it.
SELECT bench_one(
  'DISTINCT: A(x,y,z) ⋈ B(x,y,z), DISTINCT x',
  'SELECT DISTINCT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y AND a.z = b.z');

-- (10) Multi-key GROUP BY (x, y): one output row per (x, y) pair.
-- Stresses the head-Var on first_member path.
SELECT bench_one(
  'multi-key GROUP BY (x, y): A ⋈ B sharing (x,y,z)',
  'SELECT a.x AS x, a.y AS y, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y AND a.z = b.z
    GROUP BY a.x, a.y');

-- (11) Head Var beyond root (a.y) in the output, with only the
-- root-class join in WHERE. Exercises the singleton head-Var on
-- non-first-member path.
SELECT bench_one(
  'head Var output: SELECT a.x, a.y, prov',
  'SELECT a.x AS x, a.y AS y, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y
    GROUP BY a.x, a.y');

-- (12) Atom-local WHERE qual on B (b.z > 1). The detector splits
-- this conjunct into bench_b''s pushed_quals; the inner B wrap
-- carries the filter, and only matching rows reach the outer plus.
SELECT bench_one(
  'atom-local qual: b.z > 1 pushed into B''s wrap',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y AND a.z = b.z AND b.z > 1
    GROUP BY a.x');

-- (13) Cascading peel: A,B share (x,y,z); C shares (x,y) only; D
-- shares x only. Detector peels x → y → z across three nested
-- recursive re-entries.
SELECT bench_one(
  'cascading peel: A,B(x,y,z) ⋈ C(x,y) ⋈ D(x)',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c, bench_d d
    WHERE a.x = b.x AND a.x = c.x AND a.x = d.x
      AND a.y = b.y AND a.y = c.y AND a.z = b.z
    GROUP BY a.x');

-- (14) 2-component 3-atom: A,B share (x,y,z); D shares x only.
-- Two components: {A,B} (rooted at y) and {D} (single-atom).
SELECT bench_one(
  '2-comp 3-atom: (A ⋈ B on x,y,z) ⋈ D on x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_d d
    WHERE a.x = b.x AND a.x = d.x AND a.y = b.y AND a.z = b.z
    GROUP BY a.x');

-- (15) Transitivity catcher (production-bench-class regression).
-- Multi-component 4-atom where user-written WHERE only equates
-- root via A; the rewriter must synthesise the missing intra-
-- group equalities (b.x=c.x via the {B,C} group, d.x=e.x via
-- {D,E}) in inner_quals; otherwise the inner wraps lose the
-- per-x granularity and the rewritten Shapley sums diverge
-- from the baseline.
SELECT bench_one(
  'transitivity: 2 components, root via A only (a.x = each other.x)',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_d d, bench_e e
    WHERE a.x = b.x AND a.x = d.x AND a.x = e.x
      AND a.y = b.y AND a.z = b.z AND d.w = e.w
    GROUP BY a.x');

-- ----------------------------------------------------------------------
-- Report.
-- ----------------------------------------------------------------------

\echo
\echo '======================================================================='
\echo 'Safe-query rewrite benchmark (Shapley value evaluation)'
\echo '======================================================================='
SELECT
  shape,
  rows_off,
  rows_on,
  round(sum_p_off::numeric, 6)    AS sum_p_off,
  round(sum_p_on::numeric,  6)    AS sum_p_on,
  round(abs_diff::numeric, 9)     AS abs_sum_diff,
  round(off_secs::numeric, 4)     AS off_secs,
  round(on_secs::numeric,  4)     AS on_secs,
  round(speedup::numeric,  2)     AS speedup
FROM bench_results
ORDER BY shape;

-- Cleanup.
DROP FUNCTION bench_one(text, text);
DROP TABLE bench_a, bench_b, bench_c, bench_d, bench_e;
