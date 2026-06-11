-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_onedp_bench.sql
--
-- Per-answer joint-width evaluation: the full TOP-DOWN single DP
-- (ucq_joint_answers_onedp) vs the per-binding and shared-decomposition
-- multiple passes.
--
-- The per-answer query  q(x) :- R(x), S(x,y), T(y)  over private-witness
-- data (each x has w independent S/T witnesses) has one answer per x.  We
-- time three ways of computing all k answers, all of which agree:
--
--   (1) per-binding (ucq_joint_answers)       -- 1 gather + k full compiles
--       (each rebuilds the encoding + decomposition + DP).
--   (2) swept       (ucq_joint_answers_swept)  -- 1 gather + 1 decomposition
--       + k head-pinned DP sweeps (shares only the decomposition).
--   (3) onedp       (ucq_joint_answers_onedp)  -- 1 gather + 1 decomposition
--       + ONE sweep that emits one circuit root per answer.
--
-- (1) and (2) are ~quadratic in k (each of the k passes sweeps all k*w
-- facts); (3) is ~linear (each fact is processed once), so the single DP
-- pulls away as k grows.
--
-- Run:  createdb ucq1dp && psql ucq1dp -X -f this.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_onedp_bench CASCADE;
CREATE SCHEMA ucq_onedp_bench;
SET search_path TO ucq_onedp_bench, provsql, public;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION onedp_bench(k int, w int, p float8)
RETURNS TABLE(answers int, witnesses_per_answer int,
              per_binding_ms numeric, swept_ms numeric, onedp_ms numeric,
              all_agree bool)
AS $$
DECLARE
  t0 timestamptz;
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[]; heads int[];
  q jsonb := '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}';
  n_pb int; n_sw int; n_od int; n_agree int;
BEGIN
  EXECUTE 'DROP TABLE IF EXISTS r,s,t CASCADE';
  EXECUTE 'CREATE TABLE r(x int)';
  EXECUTE 'CREATE TABLE s(x int, y int)';
  EXECUTE 'CREATE TABLE t(y int)';
  PERFORM add_provenance('r'); PERFORM add_provenance('s'); PERFORM add_provenance('t');
  EXECUTE format('INSERT INTO r SELECT g FROM generate_series(1,%s) g', k);
  EXECUTE format('INSERT INTO s SELECT g, g*1000+j FROM generate_series(1,%s) g, generate_series(1,%s) j', k, w);
  EXECUTE format('INSERT INTO t SELECT g*1000+j FROM generate_series(1,%s) g, generate_series(1,%s) j', k, w);
  PERFORM set_prob(provsql, p) FROM r;
  PERFORM set_prob(provsql, p) FROM s;
  PERFORM set_prob(provsql, p) FROM t;
  SET provsql.provenance = 'boolean';

  -- gather the columnar arrays ONCE.
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

  -- (1) per-binding.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bp AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers(
      q, ARRAY[0], heads, frel, fel, far, ftok, fpr);
  per_binding_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- (2) swept (shared decomposition).
  t0 := clock_timestamp();
  CREATE TEMP TABLE bs AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_swept(
      q, ARRAY[0], heads, frel, fel, far, ftok, fpr);
  swept_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- (3) onedp (single top-down sweep, answers discovered).
  t0 := clock_timestamp();
  CREATE TEMP TABLE bo AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_onedp(
      q, ARRAY[0], frel, fel, far, ftok, fpr);
  onedp_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  SELECT count(*) INTO n_pb FROM bp;
  SELECT count(*) INTO n_sw FROM bs;
  SELECT count(*) INTO n_od FROM bo;
  SELECT count(*) INTO n_agree FROM bp
    JOIN bs ON bp.x = bs.x AND abs(bp.pr - bs.pr) < 1e-9
    JOIN bo ON bp.x = bo.x AND abs(bp.pr - bo.pr) < 1e-9;
  answers := k; witnesses_per_answer := w;
  all_agree := (n_pb = k AND n_sw = k AND n_od = k AND n_agree = k);
  DROP TABLE bp; DROP TABLE bs; DROP TABLE bo; DROP TABLE hf;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### Per-answer joint-width: top-down single DP vs multiple passes.'
\echo '###   per_binding_ms = 1 gather + k full compiles'
\echo '###   swept_ms       = 1 gather + 1 decomposition + k pinned DPs'
\echo '###   onedp_ms       = 1 gather + 1 decomposition + 1 sweep (k roots)'
\echo '###   all three agree on every answer.'
\echo '###'
SELECT * FROM onedp_bench(16,   2, 0.5);
SELECT * FROM onedp_bench(64,   2, 0.5);
SELECT * FROM onedp_bench(256,  2, 0.5);
SELECT * FROM onedp_bench(1024, 2, 0.5);
SELECT * FROM onedp_bench(4096, 2, 0.5);

DROP SCHEMA ucq_onedp_bench CASCADE;
