-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_single_sweep_bench.sql
--
-- Per-answer joint-width evaluation: SINGLE PASS vs MULTIPLE PASSES.
--
-- The per-answer query  q(x) :- R(x), S(x,y), T(y)  over private-witness
-- data (each x has w independent S/T witnesses) has one answer per x.  We
-- time three ways of computing all k answers, all of which agree:
--
--   (1) per-group transparent  --  SELECT x, probability_evaluate(
--       provenance()) ... GROUP BY x  --  the planner substitutes a
--       head-pinned joint-width call PER GROUP, so the relations are
--       RE-GATHERED, re-encoded, re-decomposed and re-evaluated k times:
--       k gathers + k compiles.
--   (2) ucq_joint_answers       --  per-binding: the facts are gathered
--       ONCE, then a fresh Sel-pinned compile runs per answer:
--       1 gather + k compiles (each rebuilds encoding + decomposition).
--   (3) ucq_joint_answers_swept --  the single sweep: the encoding and the
--       tree decomposition are built ONCE and each answer is a head-pinned
--       bottom-up sweep: 1 gather + 1 decompose + k pinned sweeps.
--
-- Run:  createdb ucqsweep && psql ucqsweep -X -f this.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_sweep_bench CASCADE;
CREATE SCHEMA ucq_sweep_bench;
SET search_path TO ucq_sweep_bench, provsql, public;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION sweep_bench(k int, w int, p float8,
                                       do_transparent bool DEFAULT true)
RETURNS TABLE(answers int, witnesses_per_answer int,
              transparent_ms numeric, per_binding_ms numeric, swept_ms numeric,
              all_agree bool)
AS $$
DECLARE
  t0 timestamptz;
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[]; heads int[];
  q jsonb := '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}';
  n_pb int; n_sw int; n_agree int;
BEGIN
  EXECUTE 'DROP TABLE IF EXISTS r,s,t CASCADE';
  EXECUTE 'CREATE TABLE r(x int)';
  EXECUTE 'CREATE TABLE s(x int, y int)';
  EXECUTE 'CREATE TABLE t(y int)';
  PERFORM add_provenance('r'); PERFORM add_provenance('s'); PERFORM add_provenance('t');
  -- x in 1..k ; each x has w PRIVATE witnesses y = x*1000 + j (tw 1 forest).
  EXECUTE format('INSERT INTO r SELECT g FROM generate_series(1,%s) g', k);
  EXECUTE format('INSERT INTO s SELECT g, g*1000+j FROM generate_series(1,%s) g, generate_series(1,%s) j', k, w);
  EXECUTE format('INSERT INTO t SELECT g*1000+j FROM generate_series(1,%s) g, generate_series(1,%s) j', k, w);
  PERFORM set_prob(provsql, p) FROM r;
  PERFORM set_prob(provsql, p) FROM s;
  PERFORM set_prob(provsql, p) FROM t;
  SET provsql.provenance = 'boolean';

  -- (1) per-group transparent: re-gathers per answer.  This path runs k
  -- full planner-substituted gather+compile cycles, so it is the slow one;
  -- skip it (do_transparent=false) at large k and compare (2) vs (3) alone.
  IF do_transparent THEN
    t0 := clock_timestamp();
    CREATE TEMP TABLE bt AS
      SELECT r.x AS x, probability_evaluate(provenance()) AS pr
        FROM r, s, t WHERE r.x = s.x AND s.y = t.y GROUP BY r.x;
    transparent_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);
  ELSE
    transparent_ms := NULL;
  END IF;

  -- gather the columnar arrays ONCE for (2) and (3).
  SET provsql.active = off;
  CREATE TEMP TABLE hf AS
    SELECT 0 rel, ARRAY[x] el, 1 ar, provsql tk FROM r
    UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM s
    UNION ALL SELECT 2, ARRAY[y], 1, provsql FROM t;
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn), array_agg(tk ORDER BY rn)
    INTO frel, far, ftok FROM (SELECT *, row_number() OVER () rn FROM hf) z;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel
    FROM (SELECT row_number() OVER () rn, u.e, u.idx
          FROM hf f, LATERAL unnest(f.el) WITH ORDINALITY u(e,idx)) s;
  SELECT array_agg(p ORDER BY rn) INTO fpr
    FROM (SELECT row_number() OVER () rn FROM hf) z;
  SELECT array_agg(DISTINCT x ORDER BY x) INTO heads FROM r;
  SET provsql.active = on;

  -- (2) per-binding: one gather, k Sel-pinned compiles.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bp AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers(
      q, ARRAY[0], heads, frel, fel, far, ftok, fpr);
  per_binding_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- (3) single sweep: one gather, one decompose, k pinned sweeps.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bs AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_swept(
      q, ARRAY[0], heads, frel, fel, far, ftok, fpr);
  swept_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- agreement (to 1e-9): swept vs per-binding always; vs transparent too
  -- when it was run.
  SELECT count(*) INTO n_pb FROM bp;
  SELECT count(*) INTO n_sw FROM bs;
  SELECT count(*) INTO n_agree FROM bp
    JOIN bs ON bp.x = bs.x AND abs(bp.pr - bs.pr) < 1e-9;
  answers := k; witnesses_per_answer := w;
  all_agree := (n_pb = k AND n_sw = k AND n_agree = k);
  IF do_transparent THEN
    SELECT count(*) INTO n_agree FROM bt
      JOIN bp ON bt.x = bp.x AND abs(bt.pr - bp.pr) < 1e-9;
    all_agree := all_agree AND (n_agree = k);
    DROP TABLE bt;
  END IF;
  DROP TABLE bp; DROP TABLE bs; DROP TABLE hf;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### Per-answer joint-width: single pass (swept) vs multiple passes.'
\echo '###   transparent_ms = k gathers + k compiles (re-gather per group)'
\echo '###   per_binding_ms = 1 gather + k compiles (rebuild decomposition)'
\echo '###   swept_ms       = 1 gather + 1 decompose + k pinned sweeps'
\echo '###   all three agree on every answer (transparent skipped at large k).'
\echo '###'
-- Small k: all three methods, including the slow re-gather-per-group path.
SELECT * FROM sweep_bench(16,  2, 0.5);
SELECT * FROM sweep_bench(64,  2, 0.5);
SELECT * FROM sweep_bench(128, 2, 0.5);
-- Large k: skip the O(k) re-gather transparent path, isolate (2) vs (3).
SELECT * FROM sweep_bench(256,  2, 0.5, false);
SELECT * FROM sweep_bench(512,  2, 0.5, false);
SELECT * FROM sweep_bench(1024, 2, 0.5, false);

DROP SCHEMA ucq_sweep_bench CASCADE;
