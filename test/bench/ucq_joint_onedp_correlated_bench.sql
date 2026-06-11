-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_onedp_correlated_bench.sql
--
-- Per-answer joint-width evaluation over CORRELATED inputs: the full
-- TOP-DOWN single DP vs the shared-decomposition multiple passes.
--
-- q(x) :- R(x), S(x,y), T(y), with R(x) an INTERNAL gate (a times of two
-- shared base events) so the merged data+circuit DP is exercised, and w
-- private S/T witnesses per x.  One answer per x.  Two methods, both exact
-- and agreeing:
--
--   (1) swept  (ucq_joint_answers_swept_tracked) -- walk the tokens ONCE,
--       build the decomposition ONCE, then a head-pinned merged DP sweep
--       PER answer: 1 walk + k pinned sweeps (quadratic in k).
--   (2) onedp  (ucq_joint_answers_onedp_tracked)  -- walk ONCE, ONE merged
--       bottom-up sweep emits one root per answer (linear in k).
--
-- Run:  createdb ucq1dpc && psql ucq1dpc -X -f this.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_onedpc_bench CASCADE;
CREATE SCHEMA ucq_onedpc_bench;
SET search_path TO ucq_onedpc_bench, provsql, public;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION onedpc_bench(k int, w int, p float8)
RETURNS TABLE(answers int, witnesses_per_answer int,
              swept_ms numeric, onedp_ms numeric, all_agree bool)
AS $$
DECLARE
  t0 timestamptz;
  frel int[]; fel int[]; far int[]; ftok uuid[]; heads int[];
  q jsonb := '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}';
  n_sw int; n_od int; n_agree int;
BEGIN
  EXECUTE 'DROP TABLE IF EXISTS base,r,s,t CASCADE';
  EXECUTE 'CREATE TABLE base(id int)';
  PERFORM add_provenance('base');
  EXECUTE format('INSERT INTO base SELECT generate_series(1, %s)', 4*k*w + 4*k);
  PERFORM set_prob(provsql, p) FROM base;
  SET provsql.provenance = 'boolean';
  -- R(x) = base(2x-1) AND base(2x): an internal times gate (correlated).
  EXECUTE 'CREATE TABLE r AS SELECT g AS x FROM generate_series(1,' || k ||
          ') g, base a, base b WHERE a.id = 2*g-1 AND b.id = 2*g';
  EXECUTE 'CREATE TABLE s AS SELECT g AS x, g*1000+j AS y FROM generate_series(1,'
          || k || ') g, generate_series(1,' || w || ') j, base c '
          'WHERE c.id = 2*' || k || ' + (g-1)*' || w || ' + j';
  EXECUTE 'CREATE TABLE t AS SELECT g*1000+j AS y FROM generate_series(1,'
          || k || ') g, generate_series(1,' || w || ') j, base d '
          'WHERE d.id = 2*' || k || ' + ' || k*w || ' + (g-1)*' || w || ' + j';

  SET provsql.active = off;
  CREATE TEMP TABLE hf AS
    SELECT 0 rel, ARRAY[x] el, 1 ar, provsql tk FROM r
    UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM s
    UNION ALL SELECT 2, ARRAY[y], 1, provsql FROM t;
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn),
         array_agg(tk ORDER BY rn)
    INTO frel, far, ftok FROM (SELECT *, row_number() OVER () rn FROM hf) z;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel
    FROM (SELECT row_number() OVER () rn, u.e, u.idx
          FROM hf f, LATERAL unnest(f.el) WITH ORDINALITY u(e,idx)) s;
  SELECT array_agg(DISTINCT x ORDER BY x) INTO heads FROM r;
  SET provsql.active = on;

  -- (1) shared-decomposition, k pinned merged DP sweeps.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bs AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_swept_tracked(
      q, ARRAY[0], heads, frel, fel, far, ftok);
  swept_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- (2) onedp: one merged sweep, answers discovered.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bo AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_onedp_tracked(
      q, ARRAY[0], frel, fel, far, ftok);
  onedp_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  SELECT count(*) INTO n_sw FROM bs;
  SELECT count(*) INTO n_od FROM bo;
  SELECT count(*) INTO n_agree FROM bs
    JOIN bo ON bs.x = bo.x AND abs(bs.pr - bo.pr) < 1e-9;
  answers := k; witnesses_per_answer := w;
  all_agree := (n_sw = k AND n_od = k AND n_agree = k);
  DROP TABLE bs; DROP TABLE bo; DROP TABLE hf;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### Correlated per-answer joint-width: top-down single DP vs k pinned sweeps.'
\echo '###   swept_ms = 1 walk + 1 decomposition + k pinned merged DP sweeps'
\echo '###   onedp_ms = 1 walk + 1 decomposition + 1 merged sweep (k roots)'
\echo '###   both agree on every answer.'
\echo '###'
SELECT * FROM onedpc_bench(16,  2, 0.5);
SELECT * FROM onedpc_bench(64,  2, 0.5);
SELECT * FROM onedpc_bench(256, 2, 0.5);
SELECT * FROM onedpc_bench(512, 2, 0.5);

DROP SCHEMA ucq_onedpc_bench CASCADE;
