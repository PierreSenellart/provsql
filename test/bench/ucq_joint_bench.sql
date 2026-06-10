-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_bench.sql
--
-- Scaling benchmark for the joint-width UCQ compiler (§7.4 of the
-- joint-width-ucq-compiler spec), data-graph regime.
--
-- A path-shaped joint instance: S a chain of n edges (i, i+1), R on the
-- even nodes, T on the odd nodes, and the query H0 = R(x), S(x, y), T(y)
-- -- a query that is #P-hard in general under the Dalvi-Suciu dichotomy.
-- Because every fact is an independent gate_input, the joint graph is the
-- Gaifman graph of the facts, which here is a path: joint treewidth 1.
--
-- The compiler should therefore evaluate the exact probability with:
--   * a peak DP state count (max_states) that is FLAT in n (bounded by
--     the joint treewidth and the query, not the data size);
--   * an emitted d-D whose size is LINEAR in the number of facts;
--   * near-linear wall-clock time.
--
-- Run :
--   createdb ucqbench && psql ucqbench -X -f test/bench/ucq_joint_bench.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
SET search_path TO provsql, public;

CREATE OR REPLACE FUNCTION ucq_bench(n int)
RETURNS TABLE(nn int, ms double precision, joint_tw int,
              max_states bigint, dd_size bigint, probability double precision)
AS $$
DECLARE
  s_rel int[]; s_el int[]; s_ar int[]; s_tok uuid[]; s_pr float8[];
  r_rel int[]; r_el int[]; r_ar int[]; r_tok uuid[]; r_pr float8[];
  t_rel int[]; t_el int[]; t_ar int[]; t_tok uuid[]; t_pr float8[];
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[];
  t0 timestamptz; t1 timestamptz; st record;
BEGIN
  SELECT array_agg(1), array_agg(2), array_agg(0.5),
         array_agg(public.uuid_generate_v5(uuid_ns_provsql(),'s'||i))
    INTO s_rel, s_ar, s_pr, s_tok FROM generate_series(0,n-1) i;
  SELECT array_agg(e ORDER BY i,k) INTO s_el
    FROM generate_series(0,n-1) i, LATERAL (VALUES (1,i),(2,i+1)) v(k,e);
  SELECT array_agg(0), array_agg(1), array_agg(0.5), array_agg(i),
         array_agg(public.uuid_generate_v5(uuid_ns_provsql(),'r'||i))
    INTO r_rel, r_ar, r_pr, r_el, r_tok
    FROM generate_series(0,n) i WHERE i % 2 = 0;
  SELECT array_agg(2), array_agg(1), array_agg(0.5), array_agg(i),
         array_agg(public.uuid_generate_v5(uuid_ns_provsql(),'t'||i))
    INTO t_rel, t_ar, t_pr, t_el, t_tok
    FROM generate_series(0,n) i WHERE i % 2 = 1;
  frel := s_rel || r_rel || t_rel;  far := s_ar || r_ar || t_ar;
  fpr  := s_pr  || r_pr  || t_pr;   ftok := s_tok || r_tok || t_tok;
  fel  := s_el  || r_el  || t_el;

  t0 := clock_timestamp();
  SELECT * INTO st FROM ucq_joint_compile_stats(
    '{"disjuncts":[{"n_vars":2,"atoms":[
        {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
    frel, fel, far, ftok, fpr);
  t1 := clock_timestamp();
  nn := n; ms := round((extract(epoch from (t1-t0))*1000)::numeric,1);
  joint_tw := st.joint_treewidth; max_states := st.max_states;
  dd_size := st.dd_size; probability := st.probability;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

SELECT * FROM ucq_bench(1000);
SELECT * FROM ucq_bench(10000);
SELECT * FROM ucq_bench(100000);
SELECT * FROM ucq_bench(1000000);
