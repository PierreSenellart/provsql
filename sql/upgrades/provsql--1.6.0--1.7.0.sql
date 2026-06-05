/**
 * @file
 * @brief ProvSQL upgrade script: 1.6.0 -> 1.7.0
 *
 * 1.7.0 is a pure SQL-surface addition: no new gate types (the
 * @c provenance_gate enum is unchanged, so no @c reset_constants_cache()
 * is needed), no new composite types, casts, operators or aggregates,
 * and no in-place migration of existing tracked relations.  It adds a
 * handful of new functions, all defined in @c provsql.common.sql and
 * reproduced verbatim below:
 *
 * - Knowledge-compilation introspection surface: @c compile_to_ddnnf /
 *   @c compile_to_ddnnf_dot (machine-readable .nnf and DOT of the
 *   compiled d-DNNF), @c ddnnf_stats (jsonb structural statistics),
 *   @c tseytin_cnf (DIMACS CNF the extension feeds to external model
 *   counters) with its variable->input mapping helpers
 *   @c tseytin_cnf_mapping_json / @c tseytin_cnf_mapping, and
 *   @c tree_decomposition_dot (DOT of the min-fill decomposition).
 *
 * - @c tool_available(text): reports whether an external tool resolves
 *   on the backend's PATH (honouring @c provsql.tool_search_path),
 *   using the same resolver the compilers / counters consult.
 *
 * - @c probability_benchmark and its @c _probability_benchmark_one
 *   helper: time every probability-evaluation method on one circuit.
 *
 * - @c eval_recursive: the fixpoint driver the planner hook calls when
 *   lowering a recursive CTE over provenance-tracked relations (PG15+
 *   at the C side; the SQL function itself installs everywhere).
 *
 * It also replaces the body of the pre-existing @c circuit_subgraph
 * introspection helper: its reported @c depth is now the longest-path
 * (canonical circuit) distance from the root (@c MAX over the BFS),
 * not the shortest-path distance (@c MIN) it returned in 1.6.0.
 *
 * Every statement is @c CREATE OR REPLACE FUNCTION, so the script is
 * idempotent without explicit guards.  Functions are ordered so each
 * dependency precedes its user (@c tseytin_cnf_mapping_json before
 * @c tseytin_cnf_mapping, @c _probability_benchmark_one before
 * @c probability_benchmark).
 */

-- This script shipped without the SET below, so every function it
-- creates landed in the session's default schema (typically public)
-- instead of provsql; the 1.8.0 -> 1.9.0 script carries the corrective
-- ALTER FUNCTION ... SET SCHEMA for databases upgraded with the old
-- file.
SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. Recursive-query fixpoint driver (called by lower_recursive_cte).
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION eval_recursive(
  body_sql  text,
  work_name text,
  colnames  text,
  coldef    text,
  max_iter  int DEFAULT 1000)
  RETURNS void AS
$$
DECLARE
  changed   boolean;        -- circuit changed structurally this round
  set_stable boolean;       -- user-column tuple set unchanged this round
  iters     int := 0;
  new_count int;            -- rows in _new this round (INSERT ROW_COUNT)
  -- Under an absorptive semiring (guaranteed by provsql.boolean_provenance,
  -- which is strictly stronger) the provenance *value* converges on cyclic data
  -- even though the circuit keeps growing structurally.  A minimal derivation
  -- cannot repeat a tuple, so it has depth <= (number of derivable tuples);
  -- after that many naive rounds the value equals the least fixpoint, and the
  -- surplus (longer, cyclic) derivations are absorbed at evaluation time.  We
  -- learn that bound from the tuple-set fixpoint and stop there, returning a
  -- circuit that is sound for absorptive evaluation only.
  boolean_mode boolean :=
    coalesce(current_setting('provsql.boolean_provenance', true)::bool, false);
  ntuples   int := NULL;    -- the bound above, set once the tuple set stabilises
BEGIN
  EXECUTE format('DROP TABLE IF EXISTS %I', work_name);
  DROP TABLE IF EXISTS _new;

  -- Tracked working table (carries provsql), initially empty, plus a scratch
  -- table of the same shape; both reused across rounds.
  EXECUTE format('CREATE TEMP TABLE %I (%s, provsql uuid)', work_name, coldef);
  EXECUTE format('CREATE TEMP TABLE _new (LIKE %I)', work_name);

  LOOP
    iters := iters + 1;
    -- Hard safety bound (also catches genuinely unbounded recursion, e.g. an
    -- unbounded counter, where even the tuple set never stabilises).
    IF iters > max_iter THEN
      RAISE EXCEPTION 'eval_recursive: no fixpoint after % rounds (cyclic data?)', max_iter;
    END IF;

    -- One round of naive evaluation: re-run the CTE body over the current
    -- working table.  INSERT targets a tracked table, so ProvSQL fills provsql.
    -- Take the row count from the INSERT itself (counting _new directly would be
    -- an aggregate over a provenance-tracked table -> an agg_token).
    EXECUTE 'TRUNCATE _new';
    EXECUTE format('INSERT INTO _new(%s) %s', colnames, body_sql);
    GET DIAGNOSTICS new_count = ROW_COUNT;

    -- Exact structural fixpoint test (content-addressed tokens => set equality).
    EXECUTE format(
      'SELECT EXISTS((TABLE _new EXCEPT TABLE %1$I) UNION ALL (TABLE %1$I EXCEPT TABLE _new))',
      work_name) INTO changed;

    -- In Boolean mode, learn the round bound from the tuple-set fixpoint (the
    -- set always stabilises after finitely many rounds, even on cyclic data).
    IF boolean_mode AND ntuples IS NULL THEN
      EXECUTE format(
        'SELECT NOT EXISTS('
        || '(SELECT %2$s FROM _new EXCEPT SELECT %2$s FROM %1$I) UNION ALL '
        || '(SELECT %2$s FROM %1$I EXCEPT SELECT %2$s FROM _new))',
        work_name, colnames) INTO set_stable;
      IF set_stable THEN
        ntuples := new_count;
      END IF;
    END IF;

    -- Copy _new into the working table (tracked -> tracked carries the tokens).
    EXECUTE format('TRUNCATE %I', work_name);
    EXECUTE format('INSERT INTO %1$I(%2$s) SELECT %2$s FROM _new', work_name, colnames);

    -- Structural fixpoint: done (acyclic / fully converged) -- sound for any
    -- semiring.
    EXIT WHEN NOT changed;

    -- Boolean mode on cyclic data: once the value-fixpoint bound is reached we
    -- stop, even though the circuit is not structurally stable.
    EXIT WHEN boolean_mode AND ntuples IS NOT NULL AND iters >= ntuples;
  END LOOP;
END
$$ LANGUAGE plpgsql SET client_min_messages = warning;

-- ----------------------------------------------------------------------
-- 2. Knowledge-compilation introspection surface.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION compile_to_ddnnf_dot(
  token UUID,
  compiler TEXT = 'd4')
  RETURNS TEXT AS
  'provsql','compile_to_ddnnf_dot' LANGUAGE C;

CREATE OR REPLACE FUNCTION compile_to_ddnnf(
  token UUID,
  compiler TEXT = 'd4')
  RETURNS TEXT AS
  'provsql','compile_to_ddnnf' LANGUAGE C;

CREATE OR REPLACE FUNCTION ddnnf_stats(
  token UUID,
  compiler TEXT = 'd4')
  RETURNS jsonb AS
  'provsql','ddnnf_stats' LANGUAGE C;

CREATE OR REPLACE FUNCTION tseytin_cnf(
  token UUID,
  weighted BOOLEAN = TRUE,
  mapping BOOLEAN = TRUE)
  RETURNS TEXT AS
  'provsql','tseytin_cnf' LANGUAGE C;

CREATE OR REPLACE FUNCTION tseytin_cnf_mapping_json(token UUID)
  RETURNS jsonb AS
  'provsql','tseytin_cnf_mapping_json' LANGUAGE C;

CREATE OR REPLACE FUNCTION tseytin_cnf_mapping(token UUID)
  RETURNS TABLE(variable INT, gate UUID, probability FLOAT8) AS $$
  SELECT variable, gate, probability
  FROM jsonb_to_recordset(tseytin_cnf_mapping_json(token))
       AS x(variable INT, gate UUID, probability FLOAT8)
  ORDER BY variable
$$ LANGUAGE SQL STABLE;

CREATE OR REPLACE FUNCTION tree_decomposition_dot(
  token UUID)
  RETURNS TEXT AS
  'provsql','tree_decomposition_dot' LANGUAGE C;

-- ----------------------------------------------------------------------
-- 3. Replacement body for the circuit_subgraph introspection helper:
--    reported depth is now the longest-path (MAX) distance from the
--    root, not the shortest-path (MIN) distance shipped in 1.6.0.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION circuit_subgraph(root UUID, max_depth INT DEFAULT 8)
  RETURNS TABLE(node UUID, parent UUID, child_pos INT, gate_type TEXT, info1 TEXT, info2 TEXT, depth INT) AS
$$
  WITH RECURSIVE bfs(node, parent, child_pos, depth) AS (
    SELECT root, NULL::UUID, NULL::INT, 0
      UNION ALL
    SELECT c.t, b.node, c.idx::INT, b.depth + 1
    FROM bfs b
    CROSS JOIN LATERAL unnest(provsql.get_children(b.node))
      WITH ORDINALITY AS c(t, idx)
    WHERE b.depth < max_depth
  ),
  -- Each node's canonical depth is its longest-path distance from the
  -- root (the standard circuit-depth notion: the longest chain of
  -- gates separating the node from the output). The recursive CTE
  -- enumerates paths up to @c max_depth, so MAX over those is the
  -- longest path of length at most @c max_depth.
  node_depth AS (
    SELECT node, MAX(depth) AS depth FROM bfs GROUP BY node
  ),
  -- All distinct (parent, child, child_pos) triples seen during the BFS.
  -- A child reached from k parents within the bound contributes k rows.
  -- Self-joins (times(x, x)) contribute one row per child position.
  edges AS (
    SELECT DISTINCT parent, node AS child, child_pos
    FROM bfs WHERE parent IS NOT NULL
  )
  SELECT
    d.node,
    e.parent,
    e.child_pos,
    provsql.get_gate_type(d.node)::TEXT,
    i.info1::TEXT,
    i.info2::TEXT,
    d.depth
  FROM node_depth d
  LEFT JOIN edges e ON e.child = d.node
  LEFT JOIN LATERAL provsql.get_infos(d.node) i ON TRUE
  ORDER BY d.depth, d.node, e.parent;
$$ LANGUAGE sql STABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 4. External-tool availability probe.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION tool_available(name TEXT)
  RETURNS BOOLEAN AS
  'provsql','tool_available' LANGUAGE C STRICT;

-- ----------------------------------------------------------------------
-- 5. Probability-method benchmark.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION _probability_benchmark_one(
  in_token UUID, in_method TEXT, in_args TEXT = NULL)
RETURNS TABLE (
  method TEXT, args TEXT,
  probability DOUBLE PRECISION,
  milliseconds DOUBLE PRECISION,
  error TEXT) AS $$
DECLARE
  t0 TIMESTAMPTZ;
  p DOUBLE PRECISION;
BEGIN
  t0 := clock_timestamp();
  BEGIN
    p := provsql.probability_evaluate(in_token, in_method, in_args);
    method := in_method;
    args := in_args;
    probability := p;
    milliseconds := EXTRACT(EPOCH FROM clock_timestamp() - t0) * 1000;
    error := NULL;
  EXCEPTION WHEN OTHERS THEN
    method := in_method;
    args := in_args;
    probability := NULL;
    milliseconds := EXTRACT(EPOCH FROM clock_timestamp() - t0) * 1000;
    error := SQLERRM;
  END;
  RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION probability_benchmark(
  token UUID,
  monte_carlo_samples INT = 10000,
  weightmc_args TEXT = '0.8;0.2')
RETURNS TABLE (
  method TEXT, args TEXT,
  probability DOUBLE PRECISION,
  milliseconds DOUBLE PRECISION,
  error TEXT) AS $$
BEGIN
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'independent');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'possible-worlds');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'tree-decomposition');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(
    token, 'monte-carlo', monte_carlo_samples::text);
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'd4');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'd4v2');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'c2d');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'minic2d');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'dsharp');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'panini-obdd');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'panini-obdd-and');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'compilation', 'panini-decdnnf');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(
    token, 'wmc', 'weightmc;' || weightmc_args);
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'wmc', 'ganak');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'wmc', 'sharpsat-td');
  RETURN QUERY SELECT * FROM provsql._probability_benchmark_one(token, 'wmc', 'dpmc');
END;
$$ LANGUAGE plpgsql;
