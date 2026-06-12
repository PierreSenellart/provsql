-- ----------------------------------------------------------------------
-- test/bench/ucq_mobius_bench.sql
--
-- The safe-UCQ Möbius-inversion route, against every circuit-level method.
--
-- The flagship is q9 / QW (Dalvi & Suciu): a union of conjunctive queries
-- that is SAFE -- PTIME data complexity -- yet only because the #P-hard
-- term of its inclusion-exclusion expansion carries a zero Möbius value
-- and cancels.  No circuit-level method keeps up, because q9 provably has
-- NO polynomial-size OBDD / FBDD / dec-DNNF (Beame, Li, Roy & Suciu): as
-- the domain N grows, the literal lineage's treewidth grows with it, so
--   * tree-decomposition rejects once the width exceeds the cap;
--   * d4 / c2d knowledge compilation has no width guarantee and blows up;
--   * possible-worlds enumeration is 2^(#tuples);
-- and the JOINT-width compiler -- which reads the structure off the data
-- graph -- also gives up, because q9's joint treewidth is unbounded on this
-- dense data (the one #P-hard shape the joint route cannot bound).  The
-- Möbius route reads the structure off the QUERY (the CNF lattice is fixed;
-- only the per-element read-once islands grow with the data), so it stays
-- polynomial: O(|D|^k) for arity k.
--
-- q9 over R(x), S1(x,y), S2(x,y), S3(x,y), T(y), with the three S relations
-- a complete bipartite graph over [N] x [N].  All tuples carry probability
-- 0.1 (a low marginal keeps the union probability away from saturation, so
-- the exact values stay informative across the range).
--
-- For each N, on the SAME data and the SAME q9 UNION query, we report:
--   * the Möbius route (default): the planner routes the existence
--     provenance to the Möbius compiler -- one gate_mobius, carrying the
--     literal lineage plus the signed coefficients -- and we split its cost
--     into COMPILE (build the lineage islands) and EVAL (the signed sweep);
--   * the literal route (provsql.mobius = off, provsql.joint_width = off):
--     provenance() reverts to the plain circuit and probability_evaluate
--     falls back to the general ladder (tree-decomposition / compilation /
--     possible-worlds).
-- The literal route is attempted only up to LITERAL_CAP below: measured on
-- this machine it finishes at N=2 (12 S-tuples, agreeing with Möbius to the
-- last digit) and already exceeds a 20 s timeout at N=3 (27 S-tuples).  Past
-- the cap it is reported as DNF -- not run -- so a single slow row does not
-- abort the whole table (statement_timeout raises query_canceled, which a
-- PL/pgSQL EXCEPTION block cannot resume from).  Möbius, by contrast,
-- returns the exact value at every size.
--
-- Run :
--   createdb mobbench && psql mobbench -X -f test/bench/ucq_mobius_bench.sql
-- ----------------------------------------------------------------------
CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
DROP SCHEMA IF EXISTS mobius_bench CASCADE;
CREATE SCHEMA mobius_bench;
SET search_path TO mobius_bench, provsql, public;

-- Backstop for the literal-circuit side: q9 has no polynomial Boolean
-- circuit, so the general methods genuinely cannot finish at scale.
SET statement_timeout = '20s';

-- The q9 / QW query as a UNION of its four prime-implicant disjuncts (each
-- arm a plain SELECT; the existence is formed by the UNION dedup).  Kept as
-- one text so the Möbius and literal runs are byte-identical.
CREATE OR REPLACE FUNCTION q9_sql() RETURNS text AS $$
  SELECT $q$
    SELECT 1 FROM r, s1 a1, s3 a3, t t3
      WHERE r.x = a1.x AND a3.y = t3.y
    UNION
    SELECT 1 FROM s1 b1, s2 b2, s3 b3, t tb
      WHERE b1.x = b2.x AND b1.y = b2.y AND b3.y = tb.y
    UNION
    SELECT 1 FROM s2 c2, s3 c3, s3 c3b, t tc
      WHERE c2.x = c3.x AND c2.y = c3.y AND c3b.y = tc.y
    UNION
    SELECT 1 FROM r d, s1 d1, s1 d1b, s2 d2, s2 d2b, s3 d3
      WHERE d.x = d1.x AND d1b.x = d2.x AND d1b.y = d2.y
        AND d2b.x = d3.x AND d2b.y = d3.y
  $q$;
$$ LANGUAGE sql IMMUTABLE;

-- Build the dense q9 instance over the domain [1..n] (the three S relations
-- a complete bipartite graph over [n] x [n]), every tuple at probability p.
CREATE OR REPLACE FUNCTION mobius_bench_data(n int, p float8)
RETURNS void AS $$
BEGIN
  PERFORM set_config('provsql.active', 'off', true);
  DROP TABLE IF EXISTS r, s1, s2, s3, t CASCADE;
  CREATE TABLE r(x int);  INSERT INTO r SELECT i FROM generate_series(1,n) i;
  CREATE TABLE t(y int);  INSERT INTO t SELECT j FROM generate_series(1,n) j;
  CREATE TABLE s1(x int, y int);
  CREATE TABLE s2(x int, y int);
  CREATE TABLE s3(x int, y int);
  INSERT INTO s1 SELECT i, j FROM generate_series(1,n) i, generate_series(1,n) j;
  INSERT INTO s2 SELECT i, j FROM generate_series(1,n) i, generate_series(1,n) j;
  INSERT INTO s3 SELECT i, j FROM generate_series(1,n) i, generate_series(1,n) j;
  PERFORM add_provenance('r');  PERFORM add_provenance('t');
  PERFORM add_provenance('s1'); PERFORM add_provenance('s2');
  PERFORM add_provenance('s3');
  PERFORM set_prob(provsql, p) FROM r;  PERFORM set_prob(provsql, p) FROM t;
  PERFORM set_prob(provsql, p) FROM s1; PERFORM set_prob(provsql, p) FROM s2;
  PERFORM set_prob(provsql, p) FROM s3;
END;
$$ LANGUAGE plpgsql;

-- One benchmark row.  Build the q9 existence provenance with Möbius on
-- (split compile vs eval), then -- only if n <= literal_cap -- build the
-- plain circuit and time the general ladder on it.
CREATE OR REPLACE FUNCTION mobius_bench_row(n int, literal_cap int,
                                            p float8 DEFAULT 0.1)
RETURNS TABLE(n_dom int, n_s_tuples bigint, gate text,
              mobius_compile_ms numeric, mobius_eval_ms numeric,
              mobius_prob float8,
              literal_ms numeric, literal_prob float8, agree text) AS $$
DECLARE
  mtok uuid; ltok uuid; t0 timestamptz; q text := q9_sql();
BEGIN
  PERFORM mobius_bench_data(n, p);
  SELECT count(*) FROM (SELECT 1 FROM s1 UNION ALL SELECT 1 FROM s2
    UNION ALL SELECT 1 FROM s3) z INTO n_s_tuples;
  n_dom := n;

  -- (1) Möbius (default).  Force the joint-width screen to decline (its
  -- joint width is unbounded on this dense data anyway; the cap just makes
  -- the routing deterministic) so q9 lands on the Möbius compiler.
  PERFORM set_config('provsql.provenance', 'boolean', true);
  PERFORM set_config('provsql.active', 'on', true);
  PERFORM set_config('provsql.joint_width', 'on', true);
  PERFORM set_config('provsql.mobius', 'on', true);
  PERFORM set_config('provsql.joint_max_treewidth', '0', true);
  t0 := clock_timestamp();
  EXECUTE format('SELECT provenance() FROM (%s) qq', q) INTO mtok;
  mobius_compile_ms := round(extract(epoch FROM clock_timestamp() - t0) * 1000, 1);
  PERFORM set_config('provsql.active', 'off', true);
  gate := get_gate_type(mtok);
  t0 := clock_timestamp();
  mobius_prob := round(probability_evaluate(mtok)::numeric, 6);
  mobius_eval_ms := round(extract(epoch FROM clock_timestamp() - t0) * 1000, 1);

  -- (2) Literal circuit + general ladder, only where it is known to finish.
  IF n <= literal_cap THEN
    PERFORM set_config('provsql.active', 'on', true);
    PERFORM set_config('provsql.mobius', 'off', true);
    PERFORM set_config('provsql.joint_width', 'off', true);
    EXECUTE format('SELECT provenance() FROM (%s) qq', q) INTO ltok;
    PERFORM set_config('provsql.active', 'off', true);
    t0 := clock_timestamp();
    literal_prob := round(probability_evaluate(ltok)::numeric, 6);
    literal_ms := round(extract(epoch FROM clock_timestamp() - t0) * 1000, 1);
    agree := CASE WHEN literal_prob = mobius_prob THEN 'exact' ELSE 'MISMATCH' END;
  ELSE
    literal_ms := NULL;
    literal_prob := NULL;
    agree := 'DNF (>20s)';
  END IF;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

\echo '== Möbius vs the general ladder on q9/QW: complete [N]x[N], all p=0.1 =='
\echo '== mobius_prob is the exact value at every N; the literal route is DNF past N=2 =='
SELECT * FROM mobius_bench_row(2, 2)
UNION ALL SELECT * FROM mobius_bench_row(3, 2)
UNION ALL SELECT * FROM mobius_bench_row(4, 2)
UNION ALL SELECT * FROM mobius_bench_row(5, 2)
UNION ALL SELECT * FROM mobius_bench_row(6, 2)
UNION ALL SELECT * FROM mobius_bench_row(8, 2)
ORDER BY n_dom;

DROP SCHEMA mobius_bench CASCADE;
