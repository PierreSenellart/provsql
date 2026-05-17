-- ----------------------------------------------------------------------
-- test/bench/safe_query_bench.sql
--
-- Benchmark for the safe-query (Boolean-rewrite) optimisation.
-- Builds a moderate-sized synthetic database, then runs a battery
-- of hierarchical CQ shapes twice each:
--
--   * provsql.boolean_provenance = OFF, probability_evaluate(prov)
--     (default method: independent first, tree-decomposition fallback)
--   * provsql.boolean_provenance = ON, probability_evaluate(prov,
--     'independent') (the rewriter must produce a read-once circuit,
--     so 'independent' should succeed directly)
--
-- For each query it prints:
--   * output cardinality under each setting (must match)
--   * sum of per-row probabilities under each setting (must match
--     modulo FP rounding for a sound rewrite)
--   * wall-clock time of each evaluation
--   * speedup ratio (off_time / on_time)
--
-- Run from psql against a fresh or existing database:
--   createdb bench && psql bench -X -f test/bench/safe_query_bench.sql
--
-- The server must have `shared_preload_libraries='provsql'` set; the
-- script installs the extension itself if necessary.  Tables and
-- helper objects use the `bench_*` prefix and are dropped at the end.
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

DROP TABLE IF EXISTS bench_a, bench_b, bench_c, bench_d, bench_e,
                     bench_bid, bench_s_pk, bench_t, bench_a_pk,
                     bench_tri_r, bench_tri_s, bench_tri_t,
                     bench_partitioned, bench_dim_p, bench_dim_c CASCADE;

-- Sizes chosen so the unrewritten knowledge-compilation fallback
-- (d4 / tree-decomposition) stays under ~10 s per shape on a
-- laptop.  bench_a is the densest atom at
-- n_x * n_y_per_x * n_z_per_y rows; the unrewritten cross-product
-- with bench_b is the actual gate-count blow-up driver.  Bump only
-- if you have time -- the OFF path scales super-linearly in atom
-- size for non-trivial shapes.
-- n_x       : distinct root-variable values
-- n_y_per_x : distinct y per x
-- n_z_per_y : distinct z per (x, y)
-- n_w_per_x : distinct w per x
\set n_x       6
\set n_y_per_x 3
\set n_z_per_y 2
\set n_w_per_x 3

-- Per-evaluation wall-clock cap so a single slow shape does not
-- monopolise the run.  The rewriter (ON) is linear in the circuit
-- size, so even the most demanding shape should finish in
-- milliseconds; the OFF path may bail with a statement-timeout
-- error and bench_one will record that in the report.
SET statement_timeout = '30s';

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

-- ------------------------------------------------------------------
-- Tables for the FD-aware extensions: constant-selection, PK FDs,
-- deterministic-relation transparency, PK-unifiable self-join, FD
-- closure (triangle CQ with two PKs), disjoint-constant self-join.
-- ------------------------------------------------------------------

-- bench_s_pk(x PK, y): middle atom of the textbook H-query under a PK.
-- x is the PRIMARY KEY, so the FD x -> y lets the rewriter drop S from
-- atoms(y); the FD-aware atom-sets become {R, S} and {T}, disjoint.
CREATE TABLE bench_s_pk (x int PRIMARY KEY, y int);
INSERT INTO bench_s_pk
  SELECT x, x FROM generate_series(1, :n_x) AS x;

-- bench_t(y): tail atom of the H-query; multiple rows per y to load
-- the OFF path.
CREATE TABLE bench_t (y int);
INSERT INTO bench_t
  SELECT y FROM generate_series(1, :n_x)        AS y,
                 generate_series(1, :n_y_per_x);

-- bench_a_pk(x PK, y, z): for the PK-unifiable self-join shape.  One
-- row per x; r1, r2 over bench_a_pk unify on PK and the remaining
-- R-S join is hierarchical.
CREATE TABLE bench_a_pk (x int PRIMARY KEY, y int, z int);
INSERT INTO bench_a_pk
  SELECT x, ((x - 1) % :n_y_per_x) + 1, ((x - 1) % :n_z_per_y) + 1
    FROM generate_series(1, :n_x) AS x;

-- bench_tri_{r,s,t}: triangle CQ with PKs on R(a) and T(c).  PK FDs
-- on both endpoints make atoms(a) = {R}, atoms(b) = {S},
-- atoms(c) = {S, T}: pairwise nested-or-disjoint, hierarchical via
-- the FD closure.
CREATE TABLE bench_tri_r (a int PRIMARY KEY, b int);
CREATE TABLE bench_tri_s (b int, c int);
CREATE TABLE bench_tri_t (c int PRIMARY KEY, a int);
INSERT INTO bench_tri_r
  SELECT x, x FROM generate_series(1, :n_x) AS x;
INSERT INTO bench_tri_s
  SELECT b, b FROM generate_series(1, :n_x)        AS b,
                    generate_series(1, :n_y_per_x);
INSERT INTO bench_tri_t
  SELECT x, x FROM generate_series(1, :n_x) AS x;

-- bench_partitioned(x, kind): for the disjoint-constant self-join.
-- Two RTEs over the relation with mutually exclusive @c kind values
-- carry disjoint tuple-sets, so the rewriter certifies the shared-
-- relid group and emits each as its own DISTINCT wrap.
CREATE TABLE bench_partitioned (x int, kind char(1));
INSERT INTO bench_partitioned
  SELECT x, k
    FROM generate_series(1, :n_x)       AS x,
         (VALUES ('A'), ('B'))          AS kk(k),
         generate_series(1, :n_y_per_x);

-- bench_dim_p, bench_dim_c: deterministic dimensions for the star-
-- schema benchmark.  No @c add_provenance call, so the rewriter sees
-- them as deterministic atoms (transparent under the dissociation
-- argument) and drops them from every class's FD-aware atom set.
CREATE TABLE bench_dim_p (pid int PRIMARY KEY, cat text);
CREATE TABLE bench_dim_c (cid int PRIMARY KEY, region text);
INSERT INTO bench_dim_p
  SELECT x, 'A' FROM generate_series(1, :n_x) AS x;
INSERT INTO bench_dim_c
  SELECT y, 'EU' FROM generate_series(1, :n_y_per_x) AS y;

-- bench_bid(x, k): block-correlated atom.  add_provenance + then
-- repair_key on @c x: rows sharing the same @c x form a mulinput
-- block (mutually exclusive draws), giving a BID atom for the
-- safe-query rewriter to exercise its BID block-key alignment
-- path against.
CREATE TABLE bench_bid AS
  SELECT x, k
    FROM generate_series(1, :n_x)        AS x,
         generate_series(1, :n_y_per_x)  AS k;

\echo 'Building provenance and setting random probabilities...'
SELECT add_provenance('bench_a');
SELECT add_provenance('bench_b');
SELECT add_provenance('bench_c');
SELECT add_provenance('bench_d');
SELECT add_provenance('bench_e');
SELECT add_provenance('bench_s_pk');
SELECT add_provenance('bench_t');
SELECT add_provenance('bench_a_pk');
SELECT add_provenance('bench_tri_r');
SELECT add_provenance('bench_tri_s');
SELECT add_provenance('bench_tri_t');
SELECT add_provenance('bench_partitioned');
-- Note: bench_dim_p / bench_dim_c are intentionally NOT
-- provenance-tracked (deterministic-relation transparency relies on
-- them carrying probability-1 tuples).
-- bench_bid: block-correlated leaves per x.  repair_key replaces
-- each row's gate_input with a gate_mulinput pointing at a shared
-- per-x key gate, so rows with the same x are mutually exclusive
-- (P(row) = 1/n_y_per_x by construction).
SELECT repair_key('bench_bid', 'x');

DO $$ BEGIN
  PERFORM set_prob(provsql, random()) FROM bench_a;
  PERFORM set_prob(provsql, random()) FROM bench_b;
  PERFORM set_prob(provsql, random()) FROM bench_c;
  PERFORM set_prob(provsql, random()) FROM bench_d;
  PERFORM set_prob(provsql, random()) FROM bench_e;
  PERFORM set_prob(provsql, random()) FROM bench_s_pk;
  PERFORM set_prob(provsql, random()) FROM bench_t;
  PERFORM set_prob(provsql, random()) FROM bench_a_pk;
  PERFORM set_prob(provsql, random()) FROM bench_tri_r;
  PERFORM set_prob(provsql, random()) FROM bench_tri_s;
  PERFORM set_prob(provsql, random()) FROM bench_tri_t;
  PERFORM set_prob(provsql, random()) FROM bench_partitioned;
END $$;

-- One row per benchmarked query: the OFF and ON output cardinalities
-- (must agree for a sound rewrite), the sum of per-row probabilities
-- under each setting (must also agree -- per-row probabilities are
-- the same when the rewrite is sound, so the sums coincide; sums are
-- used instead of per-row max-diff because the bench has no
-- knowledge of the user-visible row key for multi-key shapes), the
-- absolute sum-diff, the wall-clock time of each evaluation, and
-- the speedup ratio.
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
-- in bench_results.  Soundness is asserted via output cardinality
-- (rows_off == rows_on) and per-side probability sum (sum_p_off ==
-- sum_p_on modulo FP precision).  Sums are cardinality-agnostic so
-- the bench does not need to know the user-visible row key for
-- multi-key shapes (where joining bench_off and bench_on on @c x
-- alone would cross-product output rows and inflate every
-- per-row metric).  When either side hits @c statement_timeout (or
-- raises any other exception), @c bench_one records NULLs in the
-- result columns it could not compute and continues with the next
-- shape rather than aborting the whole run.
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
    EXECUTE 'CREATE TEMP TABLE bench_off_p  AS
             SELECT probability_evaluate(prov) AS p FROM bench_off';
    t1 := clock_timestamp();
    off_secs := EXTRACT(EPOCH FROM (t1 - t0));
    off_ok := true;
  EXCEPTION WHEN OTHERS THEN
    off_secs := EXTRACT(EPOCH FROM (clock_timestamp() - t0));
    RAISE NOTICE 'OFF % failed after %s: %', shape, off_secs, SQLERRM;
  END;

  -- ON
  SET LOCAL provsql.boolean_provenance = on;
  EXECUTE format('CREATE TEMP TABLE bench_on  AS %s', qry);
  PERFORM remove_provenance('bench_on');
  BEGIN
    t0 := clock_timestamp();
    EXECUTE 'CREATE TEMP TABLE bench_on_p  AS
             SELECT probability_evaluate(prov, ''independent'') AS p
               FROM bench_on';
    t1 := clock_timestamp();
    on_secs := EXTRACT(EPOCH FROM (t1 - t0));
    on_ok := true;
  EXCEPTION WHEN OTHERS THEN
    on_secs := EXTRACT(EPOCH FROM (clock_timestamp() - t0));
    RAISE NOTICE 'ON % failed after %s: %', shape, on_secs, SQLERRM;
  END;

  IF off_ok THEN
    SELECT count(*), sum(p) INTO rows_off, sum_p_off FROM bench_off_p;
  END IF;
  IF on_ok THEN
    SELECT count(*), sum(p) INTO rows_on, sum_p_on FROM bench_on_p;
  END IF;

  INSERT INTO bench_results VALUES (
    shape, rows_off, rows_on, sum_p_off, sum_p_on,
    CASE WHEN off_ok AND on_ok THEN abs(sum_p_off - sum_p_on) END,
    off_secs, on_secs,
    CASE WHEN on_ok AND off_ok AND on_secs > 0
         THEN off_secs / on_secs ELSE NULL END);

  DROP TABLE bench_off;
  IF off_ok THEN DROP TABLE bench_off_p; END IF;
  DROP TABLE bench_on;
  IF on_ok  THEN DROP TABLE bench_on_p;  END IF;
END;
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- Shapes to benchmark.  Each is a hierarchical CQ that the safe-query
-- rewriter handles end-to-end.
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
-- root-class join in WHERE.  Exercises the singleton head-Var on
-- non-first-member path.
SELECT bench_one(
  'head Var output: SELECT a.x, a.y, prov',
  'SELECT a.x AS x, a.y AS y, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y
    GROUP BY a.x, a.y');

-- (12) Atom-local WHERE qual on B (b.z > 1).  The detector splits
-- this conjunct into bench_b''s pushed_quals; the inner B wrap
-- carries the filter, and only matching rows reach the outer plus.
SELECT bench_one(
  'atom-local qual: b.z > 1 pushed into B''s wrap',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b
    WHERE a.x = b.x AND a.y = b.y AND a.z = b.z AND b.z > 1
    GROUP BY a.x');

-- (13) Cascading peel: A,B share (x,y,z); C shares (x,y) only; D
-- shares x only.  Detector peels x → y → z across three nested
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

-- (15) BID 2-atom: bench_bid (block_key = x) ⋈ A on x.  The BID
-- alignment check requires the BID atom's wrap to expose the
-- block_key columns -- here just x -- and proj_slots does so via
-- the root slot.  Per output x: mulinput leaves of bench_bid
-- (rows with same x are mutually exclusive) × plus over (y, z)
-- of A's leaves.
SELECT bench_one(
  'BID: bench_bid (block_key=x) ⋈ A on x',
  'SELECT bb.x AS x, provenance() AS prov
     FROM bench_bid bb, bench_a a
    WHERE bb.x = a.x
    GROUP BY bb.x');

-- (15b) BID 3-atom mixed with TID + 2-component: bench_bid ⋈ A ⋈
-- (D ⋈ E on w).  bench_bid joins on x with A (which adds an inner
-- (y, z) plus) and the (D, E) component (which adds an inner w
-- plus).  Exercises BID + multi-component together.
SELECT bench_one(
  'BID + 2-comp: bench_bid ⋈ A ⋈ (D ⋈ E)',
  'SELECT bb.x AS x, provenance() AS prov
     FROM bench_bid bb, bench_a a, bench_d d, bench_e e
    WHERE bb.x = a.x AND bb.x = d.x AND bb.x = e.x AND d.w = e.w
    GROUP BY bb.x');

-- (16) Transitivity catcher (production-bench-class regression).
-- Multi-component 4-atom where user-written WHERE only equates
-- root via A; the rewriter must synthesise the missing intra-
-- group equalities (b.x=c.x via the {B,C} group, d.x=e.x via
-- {D,E}) in inner_quals -- otherwise the inner wraps lose the
-- per-x granularity and the rewritten probability diverges from
-- the baseline.
SELECT bench_one(
  'transitivity: 2 components, root via A only (a.x = each other.x)',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_b b, bench_d d, bench_e e
    WHERE a.x = b.x AND a.x = d.x AND a.x = e.x
      AND a.y = b.y AND a.z = b.z AND d.w = e.w
    GROUP BY a.x');

-- ----------------------------------------------------------------------
-- FD-aware extensions: shapes that the textbook hierarchical
-- detector refuses on raw atom-sets but the FD-induced reductions
-- (constant-pinning, PK FDs, deterministic-relation transparency,
-- PK-unifiable / disjoint-constant self-joins) accept.
-- ----------------------------------------------------------------------

-- (17) Constant selection.  Textbook H-query R(x), S(x,y), T(y) with
-- a constant-pinned class via @c B.x = 2.  The pre-pass propagates
-- the literal through the equijoin closure and drops the
-- now-redundant @c a.x = b.x conjunct, so @c A becomes its own
-- component and the multi-component path factors it out.
SELECT bench_one(
  'const-sel: A(x) ⋈ B(x,y) ⋈ C(y) WHERE B.x = 2',
  'SELECT 1 AS k, provenance() AS prov
     FROM bench_a a, bench_b b, bench_c c
    WHERE a.x = b.x AND b.y = c.y AND b.x = 2
    GROUP BY 1');

-- (18) PK FD.  Textbook H-query R(x), S(x,y), T(y) with PRIMARY KEY
-- on @c S.x.  The FD x → y inside S lets the detector drop S from
-- atoms(y); FD-aware atom-sets become disjoint and the rewriter
-- emits the read-once shape via per-atom anchors.
SELECT bench_one(
  'PK FD: A(x) ⋈ S_PK(x,y) ⋈ T(y)',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_s_pk s, bench_t t
    WHERE a.x = s.x AND s.y = t.y
    GROUP BY a.x');

-- (19) Deterministic-relation transparency.  Star schema with a
-- probabilistic fact (@c bench_a) and two deterministic dimensions
-- (no @c add_provenance call).  Transparency drops both dimensions
-- from atom-set membership, collapsing the query to a single
-- probabilistic atom.
SELECT bench_one(
  'deterministic dims: A ⋈ DetP(x=pid) ⋈ DetC(y=cid), filtered',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a, bench_dim_p p, bench_dim_c c
    WHERE a.x = p.pid AND a.y = c.cid
      AND p.cat = ''A'' AND c.region = ''EU''
    GROUP BY a.x');

-- (20) PK-unifiable self-join.  Three RTEs where @c r1, @c r2 over
-- @c bench_a_pk are unified through the PK on @c x; the surviving
-- single R atom forms a hierarchical join with @c S on (y, z).
SELECT bench_one(
  'PK self-join: R_PK r1, r2 ⋈ A on (y, z)',
  'SELECT r1.x AS x, provenance() AS prov
     FROM bench_a_pk r1, bench_a_pk r2, bench_a a
    WHERE r1.x = r2.x AND r1.y = a.y AND r2.z = a.z
    GROUP BY r1.x');

-- (21) FD closure: triangle CQ with two PKs.  Raw atom-sets are
-- pairwise neither nested nor disjoint -- the textbook check
-- bails.  Applying both PK FDs (a → b in R and c → a in T) yields
-- atoms_fd(a) = {R}, atoms_fd(b) = {S}, atoms_fd(c) = {S, T}:
-- pairwise nested-or-disjoint, hierarchical via per-atom anchors.
SELECT bench_one(
  'FD closure: triangle CQ R(a PK,b) ⋈ S(b,c) ⋈ T(c PK,a)',
  'SELECT 1 AS k, provenance() AS prov
     FROM bench_tri_r r, bench_tri_s s, bench_tri_t t
    WHERE r.b = s.b AND s.c = t.c AND t.a = r.a
    GROUP BY 1');

-- (22) Disjoint-constant self-join.  Two RTEs over @c bench_partitioned
-- with mutually exclusive @c kind values.  Their tuple-sets are
-- disjoint, the rewriter certifies the shared-relid group and emits
-- each as its own DISTINCT wrap.
SELECT bench_one(
  'disjoint const self-join: R(x,kind) r1 (kind=A) ⋈ r2 (kind=B) on x',
  'SELECT 1 AS k, provenance() AS prov
     FROM bench_partitioned r1, bench_partitioned r2
    WHERE r1.kind = ''A'' AND r2.kind = ''B'' AND r1.x = r2.x
    GROUP BY 1');

-- ----------------------------------------------------------------------
-- Propagation extensions: view descent, INNER / CROSS JoinExpr
-- flattening, CTAS-derived TID inheritance.  Each of these turns
-- a query shape the OFF-path baseline still handles into an ON-path
-- read-once rewrite that used to be refused.
-- ----------------------------------------------------------------------

-- View + CTAS-derived fixtures shared by the propagation shapes.
CREATE VIEW bench_v_a AS SELECT * FROM bench_a;
CREATE TABLE bench_derived_a AS SELECT x, y, z, provsql FROM bench_a;

-- (23) View descent : the subquery-inlining pre-pass lifts a simple
-- view body into the outer rtable so the safe-query rewriter sees
-- a flat join over base relations.  Same gate factoring as the
-- 2-atom textbook shape (1), just reached through a view.
SELECT bench_one(
  'view descent: VIEW(A)(x) ⋈ B(x), GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_v_a a, bench_b b WHERE a.x = b.x GROUP BY a.x');

-- (24) ANSI INNER JOIN syntax : the JoinExpr-flattening pre-pass
-- dissolves the INNER JOIN into a flat fromlist + AND-merged ON
-- clause so the candidate gate sees the same shape as the
-- comma-style FROM A, B WHERE A.x = B.x counterpart.
SELECT bench_one(
  'INNER JOIN: A(x) JOIN B(x) ON, GROUP BY x',
  'SELECT a.x AS x, provenance() AS prov
     FROM bench_a a INNER JOIN bench_b b ON a.x = b.x
    GROUP BY a.x');

-- (25) ANSI INNER JOIN inside a subquery : flattening recurses
-- through RTE_SUBQUERY bodies before inlining, so a wrapped
-- INNER JOIN gets flattened in the inner body, then the inlining
-- pre-pass lifts the now-flat subquery into the outer rtable.
SELECT bench_one(
  'wrapped INNER JOIN: SELECT ... FROM (A JOIN B ON), GROUP BY x',
  'SELECT s.x AS x, provenance() AS prov FROM (
     SELECT a.x, a.provsql FROM bench_a a INNER JOIN bench_b b
       ON a.x = b.x
   ) s GROUP BY s.x');

-- (26) CTAS-derived TID join : bench_derived_a inherited TID +
-- ancestors = {bench_a} from the lineage hook ; joining it with
-- bench_b (ancestors = {bench_b}) clears the ancestry-disjointness
-- gate and the rewriter fires.
SELECT bench_one(
  'CTAS-derived: derived(A) ⋈ B on x, GROUP BY x',
  'SELECT d.x AS x, provenance() AS prov
     FROM bench_derived_a d, bench_b b WHERE d.x = b.x GROUP BY d.x');

-- ----------------------------------------------------------------------
-- Report.
-- ----------------------------------------------------------------------

\echo
\echo '======================================================================='
\echo 'Safe-query rewrite benchmark (independent vs default probability)'
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
DROP VIEW bench_v_a;
DROP TABLE bench_a, bench_b, bench_c, bench_d, bench_e, bench_bid,
           bench_s_pk, bench_t, bench_a_pk,
           bench_tri_r, bench_tri_s, bench_tri_t,
           bench_partitioned, bench_dim_p, bench_dim_c,
           bench_derived_a;
