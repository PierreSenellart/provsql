\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Columnar internals of the bounded-treewidth reachability route:
-- reachability_evaluate / reachability_compile_stats compile s-t
-- reachability along a tree decomposition of the *data* graph (the
-- provenance refinement of Courcelle's theorem) into a d-DNNF that is
-- deterministic and decomposable by construction, of size linear in the
-- number of edges for fixed data treewidth -- exact and linear-time on
-- cyclic graphs as well.  The user-facing route is an ordinary
-- WITH RECURSIVE reachability query under provsql.boolean_provenance
-- (see the btw_recursive test, PG15+); these columnar forms take
-- parallel arrays and are exercised here version-independently with
-- closed-form checks.

-- Helper: a chain of n edges with probability p, as columnar arrays.
CREATE OR REPLACE FUNCTION btw_chain_prob(n int, p float8)
RETURNS float8 AS $$
  SELECT reachability_evaluate(
    (SELECT array_agg(i) FROM generate_series(1,n) i),
    (SELECT array_agg(i+1) FROM generate_series(1,n) i),
    (SELECT array_agg(public.uuid_generate_v5(uuid_ns_provsql(), 'btwchain'||i))
       FROM generate_series(1,n) i),
    (SELECT array_agg(p) FROM generate_series(1,n) i),
    1, n+1, true);
$$ LANGUAGE sql;

-- Series: p^n.
SELECT round(btw_chain_prob(20, 0.9)::numeric, 6) AS series_20,
       round(0.9^20, 6) AS expected;

-- Parallel edges: 1 - (1-p)^3.
SELECT round(reachability_evaluate(
  ARRAY[1,1,1], ARRAY[2,2,2],
  ARRAY['11111111-1111-1111-1111-111111111111',
        '22222222-2222-2222-2222-222222222222',
        '33333333-3333-3333-3333-333333333333']::uuid[],
  ARRAY[0.5,0.5,0.5], 1, 2, true)::numeric, 6) AS parallel_3;

-- Undirected triangle: P(a ~ c) = P(ac) + (1-P(ac)) P(ab) P(bc) = 0.625.
SELECT round(reachability_evaluate(
  ARRAY[1,2,1], ARRAY[2,3,3],
  ARRAY['11111111-1111-1111-1111-111111111111',
        '22222222-2222-2222-2222-222222222222',
        '33333333-3333-3333-3333-333333333333']::uuid[],
  ARRAY[0.5,0.5,0.5], 1, 3, false)::numeric, 6) AS triangle_undirected;

-- The same undirected edge encoded as a mutual-reverse pair of arcs
-- sharing their provenance token.
SELECT round(reachability_evaluate(
  ARRAY[1,2, 2,3, 1,3],
  ARRAY[2,1, 3,2, 3,1],
  ARRAY['11111111-1111-1111-1111-111111111111','11111111-1111-1111-1111-111111111111',
        '22222222-2222-2222-2222-222222222222','22222222-2222-2222-2222-222222222222',
        '33333333-3333-3333-3333-333333333333','33333333-3333-3333-3333-333333333333']::uuid[],
  ARRAY[0.5,0.5, 0.5,0.5, 0.5,0.5],
  1, 3, true)::numeric, 6) AS triangle_reverse_pairs;

-- Cyclic graph (cf. the recursive test): reach(4) = 0.7416, exactly,
-- with no fixpoint machinery.
SELECT round(reachability_evaluate(
  ARRAY[1,2,3,2,3], ARRAY[2,3,2,4,4],
  ARRAY['11111111-1111-1111-1111-111111111111',
        '22222222-2222-2222-2222-222222222222',
        '33333333-3333-3333-3333-333333333333',
        '44444444-4444-4444-4444-444444444444',
        '55555555-5555-5555-5555-555555555555']::uuid[],
  ARRAY[0.9,0.8,0.5,0.6,0.7], 1, 4, true)::numeric, 6) AS cyclic_directed;

-- Trivial cases.
SELECT round(reachability_evaluate(NULL,NULL,NULL,NULL, 1, 1, true)::numeric, 6)
  AS source_equals_target;
SELECT round(reachability_evaluate(NULL,NULL,NULL,NULL, 1, 2, true)::numeric, 6)
  AS no_edges;

-- Compilation statistics on a 2 x 30 ladder (treewidth 2), undirected:
-- the emitted d-DNNF stays linear in the edge count; the structural
-- counters are heuristic- and platform-dependent, so only sanity bounds
-- are printed.
WITH edges AS (
  SELECT 2*i+1 AS s, 2*i+3 AS d, 0.9::float8 AS p,
         public.uuid_generate_v5(uuid_ns_provsql(), 'btwlad-t'||i) AS tok
    FROM generate_series(0,28) i
  UNION ALL
  SELECT 2*i+2, 2*i+4, 0.9, public.uuid_generate_v5(uuid_ns_provsql(), 'btwlad-b'||i)
    FROM generate_series(0,28) i
  UNION ALL
  SELECT 2*i+1, 2*i+2, 0.5, public.uuid_generate_v5(uuid_ns_provsql(), 'btwlad-r'||i)
    FROM generate_series(0,29) i
)
SELECT data_treewidth,
       nb_variables,
       nb_gates < 100 * nb_variables AS gates_linear,
       max_states <= 32 AS states_bounded,
       round(probability::numeric, 6) AS probability
FROM edges,
     LATERAL (SELECT array_agg(s) ss, array_agg(d) dd, array_agg(tok) tt,
                     array_agg(p) pp FROM edges) arr,
     reachability_compile_stats(arr.ss, arr.dd, arr.tt, arr.pp, 1, 60, false)
LIMIT 1;

-- Error paths.
\set VERBOSITY terse
-- Data treewidth above the supported limit (K14 has treewidth 13).
SELECT reachability_evaluate(
  (SELECT array_agg(i) FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j),
  (SELECT array_agg(j) FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j),
  (SELECT array_agg(public.uuid_generate_v5(uuid_ns_provsql(), 'k14-'||i||'-'||j))
     FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j),
  (SELECT array_agg(0.5::float8) FROM generate_series(1,14) i, generate_series(1,14) j WHERE i < j),
  1, 14, false);
-- A token shared by edges that are not mutual reverses.
SELECT reachability_evaluate(
  ARRAY[1,2], ARRAY[2,3],
  ARRAY['11111111-1111-1111-1111-111111111111',
        '11111111-1111-1111-1111-111111111111']::uuid[],
  ARRAY[0.5,0.5], 1, 3, true);
-- Mismatched array lengths.
SELECT reachability_evaluate(ARRAY[1], ARRAY[2,3],
  ARRAY['11111111-1111-1111-1111-111111111111']::uuid[], ARRAY[0.5], 1, 2, true);
\set VERBOSITY default

DROP FUNCTION btw_chain_prob(int, float8);
