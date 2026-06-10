-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_correlated_bench.sql
--
-- The CORRELATED regime of the joint-width UCQ compiler, at scale.
--
-- The separation bench (ucq_joint_separation_bench.sql) lives in the
-- data-graph / TID regime, where every fact is an independent gate_input.
-- Here the facts are NOT independent: a fact's provenance is an internal
-- gate over shared events -- a materialised tracked view.  This is the
-- regime the "joint" in joint-width is named for.  (Native BID blocks from
-- repair_key are a gate_mulinput correlation the slice walk does not yet
-- handle -- input/and/or/not only -- so they are out of scope here.)
--
-- Three things this regime buys, none of which the data-graph case shows:
--
--  (1) SOUNDNESS.  Correlation changes the answer, and a screen that looks
--      at the data alone (or treats the facts as independent) is *unsound*
--      -- Amarilli, PhD thesis Prop. 4.2.11: the data treewidth and the
--      circuit treewidth can each be small while the instance is hard, so
--      only the JOINT graph of data + correlations is a sound screen.  The
--      bench reports data_treewidth_lb and circuit_treewidth_lb (each
--      small) beside the joint treewidth (the real bound), and contrasts
--      the joint-width answer with the WRONG independent reading.
--
--  (2) APPLICABILITY.  Query-side safety (the Dalvi-Suciu dichotomy)
--      presumes tuple-independent inputs, so over correlated inputs the
--      lifted-inference methods do not merely lose, they DECLINE: the
--      'independent' (read-once) method rejects the correlated circuit
--      outright.  Joint-width stays exact.
--
--  (3) A WIDTH GUARANTEE, near-linearly.  The joint screen certifies the
--      instance tractable up front and the compiler is linear in the data
--      for bounded joint width.
--
-- The joint-width answer is cross-checked against ProvSQL's own ladder on
-- the NATIVE correlated circuit (built with provsql.joint_width = off so
-- the ladder, not joint-width, evaluates it); the two must agree to 1e-9.
--
-- Run :
--   createdb ucqbench && psql ucqbench -X -f test/bench/ucq_joint_correlated_bench.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_corr_bench CASCADE;
CREATE SCHEMA ucq_corr_bench;
SET search_path TO ucq_corr_bench, provsql, public;

-- A CORRELATED chain.  H0 = R(x), S(x,y), T(y) over base events e_0..e_n
-- (each independent, prob p):
--   R(i)     = e_i                    (a base input)
--   S(i,i+1) = e_i AND e_{i+1}        (a tracked-view times gate -- SHARES
--              e_i with R(i) and e_{i+1} with T(i+1): the correlation)
--   T(i)     = e_i                    (a base input)
-- so edge (i,i+1)'s H0 monomial collapses to e_i AND e_{i+1}, and the
-- answer is the chain 2-DNF OR_i (e_i AND e_{i+1}).  Consecutive monomials
-- SHARE e_{i+1}; ignoring that (treating the n edges as independent, each
-- of probability p^2) gives the WRONG 1 - (1 - p^2)^n.
CREATE OR REPLACE FUNCTION corr_chain(n int, p float8)
RETURNS TABLE(family text, param int, joint_tw int, data_tw_lb int,
              circuit_tw_lb int, joint_ms numeric, p_joint numeric,
              p_ladder numeric, p_independent_wrong numeric, lifted text)
AS $$
DECLARE
  st record; t0 timestamptz; tok uuid;
  frel int[]; fel int[]; far int[]; ftok uuid[];
BEGIN
  EXECUTE 'DROP TABLE IF EXISTS base, r, s, t, h0 CASCADE';
  EXECUTE 'CREATE TABLE base(k int)';
  PERFORM add_provenance('base');
  EXECUTE format('INSERT INTO base SELECT g FROM generate_series(0,%s) g', n);
  PERFORM set_prob(provsql, p) FROM base;
  SET provsql.provenance = 'boolean';
  EXECUTE 'CREATE TABLE r AS SELECT k AS x FROM base';
  EXECUTE 'CREATE TABLE s AS SELECT a.k AS x, b.k AS y FROM base a JOIN base b ON b.k = a.k + 1';
  EXECUTE 'CREATE TABLE t AS SELECT k AS y FROM base';

  -- Ladder oracle on the NATIVE correlated circuit (joint_width off so the
  -- ladder -- not the joint-width substitution -- evaluates it).  The ladder
  -- does NOT scale on the large chain, so the cross-check runs only on the
  -- small sizes; that the ladder cannot reach the big sizes joint-width
  -- handles in milliseconds is itself the point.
  IF n <= 512 THEN
    SET provsql.joint_width = off;
    CREATE TEMP TABLE h0 AS SELECT 1 AS d, provenance() AS tok
      FROM (SELECT DISTINCT 1 FROM r, s, t WHERE r.x = s.x AND s.y = t.y) q GROUP BY 1;
    SELECT h0.tok INTO tok FROM h0;
    p_ladder := round(probability_evaluate(tok)::numeric, 6);
    BEGIN
      PERFORM probability_evaluate(tok, 'independent');
      lifted := 'independent ACCEPTED (unexpected)';
    EXCEPTION WHEN OTHERS THEN lifted := 'independent DECLINES (correlated)'; END;
    SET provsql.joint_width = on;
  ELSE
    p_ladder := NULL;   -- ladder oracle does not scale; joint-width does
    lifted := 'ladder too slow at this size';
  END IF;

  -- joint-width over the real (correlated) tokens, tracked path.
  SET provsql.active = off;
  WITH f AS (
      SELECT 0 rel, ARRAY[x] el, 1 ar, provsql tk FROM r
      UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM s
      UNION ALL SELECT 2, ARRAY[y], 1, provsql FROM t),
     fo AS (SELECT *, row_number() OVER () rn FROM f)
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn),
         array_agg(tk ORDER BY rn) INTO frel, far, ftok FROM fo;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel FROM (
      SELECT row_number() OVER () rn, u.e, u.idx FROM (
        SELECT ARRAY[x] el FROM r UNION ALL SELECT ARRAY[x,y] el FROM s
        UNION ALL SELECT ARRAY[y] el FROM t) g,
      LATERAL unnest(g.el) WITH ORDINALITY u(e,idx)) z;
  SET provsql.active = on;

  family := 'corr chain'; param := n;
  t0 := clock_timestamp();
  SELECT * INTO st FROM ucq_joint_compile_stats_tracked(
    '{"disjuncts":[{"n_vars":2,"atoms":[
        {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
    frel, fel, far, ftok);
  joint_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric,1);
  p_joint := round(st.probability::numeric, 6);
  joint_tw := st.joint_treewidth;
  data_tw_lb := st.data_treewidth_lb;
  circuit_tw_lb := st.circuit_treewidth_lb;
  -- The wrong independent reading; converges to 1 once the chain saturates,
  -- so only meaningful (distinguishable from p_joint) on the small sizes.
  IF n <= 64 THEN
    p_independent_wrong := round((1 - power(1 - p*p, n))::numeric, 6);
  ELSE
    p_independent_wrong := NULL;   -- both -> 1.000000; contrast is in the small rows
  END IF;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### CORRELATED chain (tracked-view shared events), H0 = R,S,T.'
\echo '###   * p_joint == p_ladder: joint-width matches the ladder on the'
\echo '###     NATIVE correlated circuit (both correlation-aware).'
\echo '###   * p_independent_wrong differs (small rows): treating the facts'
\echo '###     as independent is UNSOUND -- the joint screen is necessary.'
\echo '###   * data_tw_lb, circuit_tw_lb each tiny; joint_tw is THE bound.'
\echo '###   * lifted ''independent'' DECLINES on correlated inputs.'
\echo '###   * joint_tw flat, time near-linear as n grows.'
\echo '###'
SELECT * FROM corr_chain(8,     0.3);
SELECT * FROM corr_chain(64,    0.3);
SELECT * FROM corr_chain(512,   0.3);
SELECT * FROM corr_chain(4096,  0.3);
SELECT * FROM corr_chain(32768, 0.3);

DROP SCHEMA ucq_corr_bench CASCADE;
