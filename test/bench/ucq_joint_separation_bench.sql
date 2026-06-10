-- ----------------------------------------------------------------------
-- test/bench/ucq_joint_separation_bench.sql
--
-- Where the joint-width UCQ compiler is the only exact method standing.
--
-- The compiler evaluates a Boolean UCQ from the DATA graph, never from the
-- materialised lineage.  Amarilli, PhD thesis Thm. 4.2.7 guarantees that a
-- bounded joint-width instance admits a linear-size bounded-width
-- provenance circuit -- but ProvSQL's executor does not build that
-- circuit, it builds the naive flat sum-of-products of the join, whose
-- treewidth is not bounded by the joint width.  So on a bounded-joint-
-- width instance of an UNSAFE query (where the safe-query read-once
-- rewrite cannot fire) the circuit-level exact methods can both fail:
--   * tree-decomposition rejects when the naive circuit's treewidth > 10
--     (TreeDecomposition::MAX_TREEWIDTH);
--   * d4 knowledge compilation has no treewidth guarantee and can blow up
--     (time/memory) even on a low-treewidth, perfectly tractable circuit.
-- The joint compiler stays linear in the data.
--
-- THREE families, all bounded joint-width (joint treewidth 1):
--
--   (1) H0 over a tree -- the CONTROL.  H0 = R(x), S(x,y), T(y), no self-
--       join, no branching join variable: each monomial is one S-edge, so
--       the naive lineage's treewidth equals the (treelike) data's.
--       tree-decomposition SUCCEEDS on the naive circuit.  The interesting
--       part is the contrast: tree-decomposition REJECTS the joint-width
--       d-D the compiler builds (a d-DNNF is read off via its certificate,
--       its primal graph is NOT bounded-treewidth), and d4 on the naive
--       circuit works small then blows up.  So the d-D is a correct,
--       linearly-evaluable certificate that the generic circuit methods
--       cannot ingest -- which is why the compiler evaluates it itself.
--
--   (2) star 2-path -- UNCONDITIONALLY unsafe.  q :- A(x), E(x,y),
--       E(y,z), B(z), E a star through one hub.  Unsafe for every input
--       (the chain x-y-z is non-hierarchical: atoms(x)={A,E}, atoms(y)=
--       {E,E}, atoms(z)={E,B}); no key makes it safe.  N^2 two-paths
--       i -> hub -> j put a K_{N,N} in the naive circuit (treewidth ~ N),
--       but the data is a star (treewidth 1).
--
--   (3) almost-safe -- safe IFF a key holds.  q :- P(x,y), Q(x,z),
--       W(x,u), T(y).  The x-y inversion through P (atoms(x)={P,Q,W},
--       atoms(y)={P,T}, overlapping at P without nesting) makes it unsafe;
--       a key P.x->y would drop P from atoms(y) and make it hierarchical
--       (ProvSQL's PK-FD pass).  A FEW key violations remove the key, so
--       the safe rewrite declines -- yet the Q x W cross-product
--       (independent of that key) puts a biclique in the naive circuit.
--       Data is a near-function (a star per x with PRIVATE z, u values),
--       so joint width stays bounded.  This is the canonical case the
--       user's "near-safe" intuition targets.
--
-- For (2) and (3) the joint-width answer is cross-checked against the
-- closed form (the per-x events are fact-disjoint, hence independent).
--
-- Run :
--   createdb ucqbench && psql ucqbench -X -f test/bench/ucq_joint_separation_bench.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS ucq_sep_bench CASCADE;
CREATE SCHEMA ucq_sep_bench;
SET search_path TO ucq_sep_bench, provsql, public;

-- Backstop only.  d4 is run solely where it is known to terminate (it can
-- exhaust memory and take the backend down, which no timeout prevents from
-- inside a function); tree-decomposition rejects via its width screen
-- almost instantly.
SET statement_timeout = '30s';

-- ----------------------------------------------------------------------
-- (1) CONTROL: H0 over a complete binary tree.
--     native tree-decomposition succeeds; d4 on the native circuit works
--     small then blows up; tree-decomposition FAILS on the joint-width
--     d-D.  run_d4 gates the d4 probe to depths where it terminates.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION sep_bench_tree(depth int, p float8, run_d4 boolean)
RETURNS TABLE(family text, param int, native_gates bigint, data_tw int,
              joint_ms numeric, p_joint numeric,
              td_native text, d4_native text, td_on_dD text)
AS $$
DECLARE
  tok_native uuid; tok_dD uuid; st record; t0 timestamptz; g0 bigint;
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[];
BEGIN
  g0 := get_nb_gates();
  EXECUTE 'DROP TABLE IF EXISTS rt, st, tt CASCADE';
  EXECUTE format($f$CREATE TABLE st AS
      SELECT i AS x, i/2 AS y FROM generate_series(2,%s) i
      UNION ALL SELECT i/2, i FROM generate_series(2,%s) i$f$,
    (1<<depth)-1, (1<<depth)-1);
  CREATE TABLE rt AS SELECT DISTINCT x FROM st;
  CREATE TABLE tt AS SELECT DISTINCT y FROM st;
  PERFORM add_provenance('rt'); PERFORM add_provenance('st');
  PERFORM add_provenance('tt');
  PERFORM set_prob(provsql, p) FROM rt;
  PERFORM set_prob(provsql, p) FROM st;
  PERFORM set_prob(provsql, p) FROM tt;

  -- Native flat lineage (semiring: no boolean joint-width substitution).
  SET provsql.provenance = 'semiring';
  CREATE TEMP TABLE hn AS SELECT 1 AS d, provenance() AS tok
    FROM (SELECT DISTINCT 1 FROM rt r, st s, tt t
           WHERE r.x = s.x AND s.y = t.y) q GROUP BY 1;
  SELECT hn.tok INTO tok_native FROM hn; DROP TABLE hn;

  -- Joint-width d-D (boolean + joint_width on: the transparent path
  -- substitutes a materialised certified d-D as the provenance token).
  SET provsql.provenance = 'boolean'; SET provsql.joint_width = on;
  CREATE TEMP TABLE hd AS SELECT 1 AS d, provenance() AS tok
    FROM (SELECT DISTINCT 1 FROM rt r, st s, tt t
           WHERE r.x = s.x AND s.y = t.y) q GROUP BY 1;
  SELECT hd.tok INTO tok_dD FROM hd; DROP TABLE hd;

  -- Joint-width data path (the compiler's own evaluation).
  SET provsql.active = off;
  WITH f AS (
      SELECT 0 rel, ARRAY[x] el, 1 ar, provsql t FROM rt
      UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM st
      UNION ALL SELECT 2, ARRAY[y], 1, provsql FROM tt),
     fo AS (SELECT *, row_number() OVER () rn FROM f)
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn),
         array_agg(t ORDER BY rn) INTO frel, far, ftok FROM fo;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel FROM (
      SELECT row_number() OVER () rn, u.e, u.idx FROM (
        SELECT ARRAY[x] el FROM rt UNION ALL SELECT ARRAY[x,y] el FROM st
        UNION ALL SELECT ARRAY[y] el FROM tt) g,
      LATERAL unnest(g.el) WITH ORDINALITY u(e,idx)) s;
  fpr := (SELECT array_agg(p) FROM generate_series(1, array_length(frel,1)));
  SET provsql.active = on;

  family := 'H0/tree (control)'; param := depth;
  native_gates := get_nb_gates() - g0;
  t0 := clock_timestamp();
  SELECT * INTO st FROM ucq_joint_compile_stats(
    '{"disjuncts":[{"n_vars":2,"atoms":[
        {"rel":0,"vars":[0]},{"rel":1,"vars":[0,1]},{"rel":2,"vars":[1]}]}]}'::jsonb,
    frel, fel, far, ftok, fpr);
  joint_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric,1);
  p_joint := round(st.probability::numeric, 6);
  data_tw := st.joint_treewidth;

  BEGIN td_native := round(probability_evaluate(tok_native,'tree-decomposition')::numeric,6)::text;
  EXCEPTION WHEN OTHERS THEN td_native := 'REJECTED'; END;
  IF run_d4 THEN
    BEGIN d4_native := round(probability_evaluate(tok_native,'compilation')::numeric,6)::text;
    EXCEPTION WHEN OTHERS THEN d4_native := 'blowup'; END;
  ELSE d4_native := 'skipped (blows up: no tw guarantee)'; END IF;
  BEGIN td_on_dD := round(probability_evaluate(tok_dD,'tree-decomposition')::numeric,6)::text;
  EXCEPTION WHEN OTHERS THEN td_on_dD := 'REJECTED ('||SQLERRM||')'; END;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- (2) UNCONDITIONAL separation: the star 2-path (always unsafe).
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION sep_bench_star(n int, p float8)
RETURNS TABLE(family text, param int, native_gates bigint, data_tw int,
              joint_ms numeric, p_joint numeric, p_closed_form numeric,
              td_native text)
AS $$
DECLARE
  tok uuid; st record; t0 timestamptz; g0 bigint; half float8;
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[];
BEGIN
  g0 := get_nb_gates();
  EXECUTE 'DROP TABLE IF EXISTS a_, e_, b_ CASCADE';
  EXECUTE format('CREATE TABLE a_ AS SELECT g AS x FROM generate_series(1,%s) g', n);
  EXECUTE format($f$CREATE TABLE e_ AS
      SELECT g AS x, 0 AS y FROM generate_series(1,%s) g
      UNION ALL SELECT 0, g FROM generate_series(1,%s) g$f$, n, n);
  EXECUTE format('CREATE TABLE b_ AS SELECT g AS z FROM generate_series(1,%s) g', n);
  PERFORM add_provenance('a_'); PERFORM add_provenance('e_');
  PERFORM add_provenance('b_');
  PERFORM set_prob(provsql, p) FROM a_;
  PERFORM set_prob(provsql, p) FROM e_;
  PERFORM set_prob(provsql, p) FROM b_;

  SET provsql.provenance = 'semiring';
  CREATE TEMP TABLE hs AS SELECT 1 AS d, provenance() AS tok
    FROM (SELECT DISTINCT 1 FROM a_ a, e_ e1, e_ e2, b_ b
           WHERE a.x = e1.x AND e1.y = e2.x AND e2.y = b.z) q GROUP BY 1;
  SELECT hs.tok INTO tok FROM hs; DROP TABLE hs;

  SET provsql.active = off;
  WITH f AS (
      SELECT 0 rel, ARRAY[x] el, 1 ar, provsql t FROM a_
      UNION ALL SELECT 1, ARRAY[x,y], 2, provsql FROM e_
      UNION ALL SELECT 2, ARRAY[z], 1, provsql FROM b_),
     fo AS (SELECT *, row_number() OVER () rn FROM f)
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn),
         array_agg(t ORDER BY rn) INTO frel, far, ftok FROM fo;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel FROM (
      SELECT row_number() OVER () rn, u.e, u.idx FROM (
        SELECT ARRAY[x] el FROM a_ UNION ALL SELECT ARRAY[x,y] el FROM e_
        UNION ALL SELECT ARRAY[z] el FROM b_) g,
      LATERAL unnest(g.el) WITH ORDINALITY u(e,idx)) s;
  fpr := (SELECT array_agg(p) FROM generate_series(1, array_length(frel,1)));
  SET provsql.active = on;

  family := 'star 2-path'; param := n;
  native_gates := get_nb_gates() - g0;
  t0 := clock_timestamp();
  SELECT * INTO st FROM ucq_joint_compile_stats(
    '{"disjuncts":[{"n_vars":3,"atoms":[{"rel":0,"vars":[0]},
        {"rel":1,"vars":[0,1]},{"rel":1,"vars":[1,2]},{"rel":2,"vars":[2]}]}]}'::jsonb,
    frel, fel, far, ftok, fpr);
  joint_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric,1);
  p_joint := round(st.probability::numeric, 6);
  data_tw := st.joint_treewidth;
  half := 1 - power(1 - p*p, n);
  p_closed_form := round((half*half)::numeric, 6);

  BEGIN td_native := round(probability_evaluate(tok,'tree-decomposition')::numeric,6)::text;
  EXCEPTION WHEN OTHERS THEN td_native := 'REJECTED ('||SQLERRM||')'; END;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- (3) ALMOST-SAFE separation: q :- P(x,y), Q(x,z), W(x,u), T(y), safe
--     IFF the key P.x->y holds, broken by a few violations.  PRIVATE z, u
--     per x keep the data a star (treewidth 1); the Q x W biclique keeps
--     the naive circuit above the tree-decomposition cap.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION sep_bench_almostsafe(nx int, kk int, p float8)
RETURNS TABLE(family text, param int, native_gates bigint, data_tw int,
              joint_ms numeric, p_joint numeric, p_closed_form numeric,
              safe_rewrite text, td_native text)
AS $$
DECLARE
  tok uuid; st record; t0 timestamptz; g0 bigint;
  frel int[]; fel int[]; far int[]; ftok uuid[]; fpr float8[];
  branch float8; pe_norm float8; pe_viol float8; v int := 2;  -- 2 violations
BEGIN
  g0 := get_nb_gates();
  EXECUTE 'DROP TABLE IF EXISTS p_, q_, w_, t_ CASCADE';
  EXECUTE format('CREATE TABLE p_ AS SELECT x, x AS y FROM generate_series(1,%s) x', nx);
  EXECUTE 'INSERT INTO p_ VALUES (1,101),(2,102)';                 -- the v=2 violations
  EXECUTE format('CREATE TABLE t_ AS SELECT y FROM generate_series(1,%s) y', nx);
  EXECUTE 'INSERT INTO t_ VALUES (101),(102)';
  EXECUTE format('CREATE TABLE q_ AS SELECT x, x*1000+j AS z FROM generate_series(1,%s) x, generate_series(1,%s) j', nx, kk);
  EXECUTE format('CREATE TABLE w_ AS SELECT x, x*1000+500+j AS u FROM generate_series(1,%s) x, generate_series(1,%s) j', nx, kk);
  PERFORM add_provenance('p_'); PERFORM add_provenance('q_');
  PERFORM add_provenance('w_'); PERFORM add_provenance('t_');
  PERFORM set_prob(provsql, p) FROM p_;
  PERFORM set_prob(provsql, p) FROM q_;
  PERFORM set_prob(provsql, p) FROM w_;
  PERFORM set_prob(provsql, p) FROM t_;

  -- Does the safe-query rewrite fire?  Probe 'independent' under boolean.
  SET provsql.provenance = 'boolean'; SET provsql.joint_width = off;
  BEGIN
    PERFORM probability_evaluate(provenance(),'independent')
      FROM (SELECT DISTINCT 1 FROM p_ p, q_ q, w_ w, t_ t
             WHERE p.x=q.x AND p.x=w.x AND p.y=t.y) s GROUP BY 1;
    safe_rewrite := 'fired (read-once)';
  EXCEPTION WHEN OTHERS THEN safe_rewrite := 'declined (unsafe)'; END;

  -- Native flat lineage under semiring.
  SET provsql.provenance = 'semiring';
  CREATE TEMP TABLE hn AS SELECT 1 AS d, provenance() AS tok
    FROM (SELECT DISTINCT 1 FROM p_ p, q_ q, w_ w, t_ t
           WHERE p.x=q.x AND p.x=w.x AND p.y=t.y) s GROUP BY 1;
  SELECT hn.tok INTO tok FROM hn; DROP TABLE hn;

  SET provsql.active = off;
  WITH f AS (
      SELECT 0 rel, ARRAY[x,y] el, 2 ar, provsql t FROM p_
      UNION ALL SELECT 1, ARRAY[x,z], 2, provsql FROM q_
      UNION ALL SELECT 2, ARRAY[x,u], 2, provsql FROM w_
      UNION ALL SELECT 3, ARRAY[y], 1, provsql FROM t_),
     fo AS (SELECT *, row_number() OVER () rn FROM f)
  SELECT array_agg(rel ORDER BY rn), array_agg(ar ORDER BY rn),
         array_agg(t ORDER BY rn) INTO frel, far, ftok FROM fo;
  SELECT array_agg(e ORDER BY rn, idx) INTO fel FROM (
      SELECT row_number() OVER () rn, u.e, u.idx FROM (
        SELECT ARRAY[x,y] el FROM p_ UNION ALL SELECT ARRAY[x,z] el FROM q_
        UNION ALL SELECT ARRAY[x,u] el FROM w_ UNION ALL SELECT ARRAY[y] el FROM t_) g,
      LATERAL unnest(g.el) WITH ORDINALITY u(e,idx)) s;
  fpr := (SELECT array_agg(p) FROM generate_series(1, array_length(frel,1)));
  SET provsql.active = on;

  family := 'almost-safe P,Q,W,T'; param := kk;
  native_gates := get_nb_gates() - g0;
  t0 := clock_timestamp();
  SELECT * INTO st FROM ucq_joint_compile_stats(
    '{"disjuncts":[{"n_vars":4,"atoms":[{"rel":0,"vars":[0,1]},
        {"rel":1,"vars":[0,2]},{"rel":2,"vars":[0,3]},{"rel":3,"vars":[1]}]}]}'::jsonb,
    frel, fel, far, ftok, fpr);
  joint_ms := round((extract(epoch from (clock_timestamp()-t0))*1000)::numeric,1);
  p_joint := round(st.probability::numeric, 6);
  data_tw := st.joint_treewidth;

  -- Closed form: per-x events are fact-disjoint -> independent.
  --   branch     = Pr[exists z Q(x,z)] = Pr[exists u W(x,u)] = 1-(1-p)^kk
  --   normal x   : Pr[exists y P(x,y) and T(y)] = p*p
  --   violation x: two y options -> 1-(1-p*p)^2
  branch  := 1 - power(1-p, kk);
  pe_norm := p*p * branch * branch;
  pe_viol := (1 - power(1 - p*p, 2)) * branch * branch;
  p_closed_form := round((1 - power(1-pe_norm, nx - v) * power(1-pe_viol, v))::numeric, 6);

  BEGIN td_native := round(probability_evaluate(tok,'tree-decomposition')::numeric,6)::text;
  EXCEPTION WHEN OTHERS THEN td_native := 'REJECTED ('||SQLERRM||')'; END;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '###'
\echo '### (1) CONTROL -- H0 over a tree (linear lineage, joint tw 1).'
\echo '###   tree-decomposition handles the NATIVE circuit; d4 handles it'
\echo '###   small then blows up; tree-decomposition REJECTS the joint-'
\echo '###   width d-D (a d-DNNF is not bounded-treewidth as a graph).'
\echo '###'
SELECT * FROM sep_bench_tree(5,  0.3, true);   -- d4 still terminates
SELECT * FROM sep_bench_tree(7,  0.3, false);  -- d4 blows up beyond depth 5
SELECT * FROM sep_bench_tree(9,  0.3, false);
SELECT * FROM sep_bench_tree(11, 0.3, false);

\echo '###'
\echo '### (2) SEPARATION -- star 2-path, UNCONDITIONALLY unsafe.'
\echo '###   no key helps; tree-decomposition rejects the naive circuit,'
\echo '###   joint-width matches the closed form.'
\echo '###'
SELECT * FROM sep_bench_star(40,  0.06);
SELECT * FROM sep_bench_star(80,  0.06);
SELECT * FROM sep_bench_star(150, 0.06);

\echo '###'
\echo '### (3) SEPARATION -- almost-safe, safe IFF the key P.x->y holds.'
\echo '###   a few violations make the safe rewrite decline; the QxW'
\echo '###   biclique rejects tree-decomposition; joint-width is exact.'
\echo '###'
SELECT * FROM sep_bench_almostsafe(6,  14, 0.5);
SELECT * FROM sep_bench_almostsafe(10, 14, 0.5);
SELECT * FROM sep_bench_almostsafe(10, 20, 0.5);

DROP SCHEMA ucq_sep_bench CASCADE;
