-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_single_sweep_correlated_bench.sql
--
-- Per-answer joint-width evaluation over CORRELATED inputs: SINGLE PASS vs
-- MULTIPLE PASSES.
--
-- The per-answer query  q(x) :- R(x), S(x,y), T(y)  has one answer per x.
-- Each R(x) is gated by an INTERNAL gate (a times of two shared base
-- events), so the inputs are genuinely correlated and the merged
-- data+circuit DP (mergedCompile) is exercised, not the data-graph fast
-- path.  We time two ways of computing all k answers, both of which agree:
--
--   (1) per-binding  -- ucq_joint_answers_swept_tracked is called ONCE PER
--       answer, each call walking the tokens, building the joint encoding
--       and the tree decomposition afresh and running one pinned DP:
--       k walks + k decompositions + k DPs.
--   (2) single sweep -- ucq_joint_answers_swept_tracked is called ONCE with
--       all k candidate heads: the tokens are walked, the encoding and the
--       tree decomposition are built ONCE, and each answer is a head-pinned
--       merged DP sweep: 1 walk + 1 decomposition + k DPs.
--
-- The two differ only by the shared walk+encode+decompose, so the ratio
-- isolates exactly what the correlated single sweep buys.
--
-- Run:  createdb ucqswc && psql ucqswc -X -f this.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_swc_bench CASCADE;
CREATE SCHEMA ucq_swc_bench;
SET search_path TO ucq_swc_bench, provsql, public;
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION swc_bench(k int, w int, p float8)
RETURNS TABLE(answers int, witnesses_per_answer int,
              per_binding_ms numeric, swept_ms numeric, all_agree bool)
AS $$
DECLARE
  t0 timestamptz;
  frel int[]; fel int[]; far int[]; ftok uuid[]; heads int[]; v int;
  q jsonb := '{"disjuncts":[{"n_vars":2,"atoms":[
       {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}';
  n_sw int; n_agree int;
BEGIN
  EXECUTE 'DROP TABLE IF EXISTS base,r,s,t CASCADE';
  -- base events: two per answer for R's internal AND, one per S and T
  -- witness; all independent inputs at probability p.
  EXECUTE 'CREATE TABLE base(id int)';
  PERFORM add_provenance('base');
  EXECUTE format('INSERT INTO base SELECT generate_series(1, %s)', 4*k*w + 4*k);
  PERFORM set_prob(provsql, p) FROM base;
  SET provsql.provenance = 'boolean';

  -- R(x) = base(2x-1) AND base(2x): an internal times gate (correlated).
  EXECUTE 'CREATE TABLE r AS SELECT g AS x FROM generate_series(1,' || k ||
          ') g, base a, base b WHERE a.id = 2*g-1 AND b.id = 2*g';
  -- S(x, x*1000+j) and T(x*1000+j): w private witnesses per x, each a single
  -- base event (a gate_input leaf), so only R carries the internal gate.
  EXECUTE 'CREATE TABLE s AS SELECT g AS x, g*1000+j AS y FROM generate_series(1,'
          || k || ') g, generate_series(1,' || w || ') j, base c '
          'WHERE c.id = 2*' || k || ' + (g-1)*' || w || ' + j';
  EXECUTE 'CREATE TABLE t AS SELECT g*1000+j AS y FROM generate_series(1,'
          || k || ') g, generate_series(1,' || w || ') j, base d '
          'WHERE d.id = 2*' || k || ' + ' || k*w || ' + (g-1)*' || w || ' + j';

  -- Columnar fact arrays (tokens, no probabilities -- the tracked path
  -- walks the circuit).  Gathered once for both methods.
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

  -- (1) per-binding: one tracked call per answer (re-walk + re-decompose).
  t0 := clock_timestamp();
  CREATE TEMP TABLE bp(x int, pr float8);
  FOREACH v IN ARRAY heads LOOP
    INSERT INTO bp
      SELECT head[1], probability FROM ucq_joint_answers_swept_tracked(
        q, ARRAY[0], ARRAY[v], frel, fel, far, ftok);
  END LOOP;
  per_binding_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  -- (2) single sweep: one tracked call, all answers share walk+decompose.
  t0 := clock_timestamp();
  CREATE TEMP TABLE bs AS
    SELECT head[1] x, probability pr FROM ucq_joint_answers_swept_tracked(
      q, ARRAY[0], heads, frel, fel, far, ftok);
  swept_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric, 1);

  SELECT count(*) INTO n_sw FROM bs;
  SELECT count(*) INTO n_agree FROM bp
    JOIN bs ON bp.x = bs.x AND abs(bp.pr - bs.pr) < 1e-9;
  answers := k; witnesses_per_answer := w;
  all_agree := (n_sw = k AND n_agree = k);
  DROP TABLE bp; DROP TABLE bs; DROP TABLE hf;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### Correlated per-answer joint-width: single pass vs multiple passes.'
\echo '###   per_binding_ms = k walks + k decompositions + k DPs'
\echo '###   swept_ms       = 1 walk + 1 decomposition + k DPs'
\echo '###   both agree on every answer.'
\echo '###'
SELECT * FROM swc_bench(16,  2, 0.5);
SELECT * FROM swc_bench(64,  2, 0.5);
SELECT * FROM swc_bench(128, 2, 0.5);
SELECT * FROM swc_bench(256, 2, 0.5);

DROP SCHEMA ucq_swc_bench CASCADE;
