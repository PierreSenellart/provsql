-- ----------------------------------------------------------------------
-- provsql 1.9.0 -> 1.10.0
--
-- New SQL surface since 1.9.0:
--   * exact two-terminal network reliability on bounded-treewidth data,
--     compiled along a tree decomposition of the data graph.  The
--     user-facing route is the ordinary WITH RECURSIVE reachability
--     query under provsql.boolean_provenance, which the query rewriter
--     lowers through the new eval_reachability driver (with fallback to
--     eval_recursive); the surface here is that driver, the
--     reachability_materialize / reachability_evaluate /
--     reachability_compile_stats columnar internals, and the
--     gather_reachability_edges helper.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

/**
 * @brief Exact reachability probability over bounded-treewidth data
 * (columnar form)
 *
 * Computes the probability that @p target is reachable from @p source in
 * the probabilistic graph given by the parallel edge arrays
 * (two-terminal network reliability).  Unlike
 * @c probability_evaluate(), which compiles the provenance circuit
 * built along the relational query plan, this compiles the query
 * along a tree decomposition of the *data* graph (in the spirit of the
 * provenance refinement of Courcelle's theorem), producing a d-DNNF
 * whose size is linear in the number of edges for data of bounded
 * treewidth.  Exact, and linear-time, on cyclic data as well -- where
 * the recursive-query fixpoint cannot terminate structurally.
 *
 * Edges are independent events.  Two array positions may share a token
 * only if they are mutual reverses (the natural encoding of an
 * undirected edge in a directed edge relation); they are then treated
 * as a single bidirectional edge.  This is an internal/testing surface:
 * the user-facing route is a plain @c WITH @c RECURSIVE reachability
 * query under @c provsql.boolean_provenance, which the query rewriter
 * compiles through @c eval_reachability() / @c reachability_materialize().
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param source the vertex reachability starts from
 * @param target the vertex whose reachability is evaluated
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_evaluate(
  sources INT[],
  destinations INT[],
  tokens UUID[],
  probabilities DOUBLE PRECISION[],
  source INT,
  target INT,
  directed BOOLEAN)
  RETURNS DOUBLE PRECISION AS
  'provsql','reachability_evaluate' LANGUAGE C IMMUTABLE PARALLEL SAFE;

/**
 * @brief Reachability probability plus compilation statistics
 * (columnar form)
 *
 * Same compilation as @c reachability_evaluate(), returning the
 * probability together with the structural statistics that
 * substantiate the bounded-treewidth guarantee: the treewidth of the
 * min-fill decomposition of the data graph, its number of bags, the
 * maximum number of dynamic-programming states at any decomposition
 * node, and the size of the emitted d-DNNF (linear in the number of
 * edges for fixed data treewidth).
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param source the vertex reachability starts from
 * @param target the vertex whose reachability is evaluated
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_compile_stats(
  IN sources INT[],
  IN destinations INT[],
  IN tokens UUID[],
  IN probabilities DOUBLE PRECISION[],
  IN source INT,
  IN target INT,
  IN directed BOOLEAN,
  OUT probability DOUBLE PRECISION,
  OUT data_treewidth INT,
  OUT nb_bags BIGINT,
  OUT max_states BIGINT,
  OUT nb_gates BIGINT,
  OUT nb_variables BIGINT)
  AS 'provsql','reachability_compile_stats'
  LANGUAGE C IMMUTABLE PARALLEL SAFE;


/**
 * @brief Compile and materialise the reachability provenance of every
 * vertex (columnar form, internal)
 *
 * All-targets variant of @c reachability_evaluate(): compiles, along a
 * tree decomposition of the data graph, one certified provenance
 * circuit per vertex reachable from some source in the all-edges-present
 * world, materialises the (shared, linear-size) circuits in the
 * provenance store -- @c plus / @c times gates carrying the d-DNNF
 * certificate, negated edges as @c monus(one, edge) -- and returns one
 * @c (vertex, token) row per such vertex.  Sources form a possibly
 * *probabilistic source set*: each source arc is gated by the source
 * tuple's token, the nil UUID marking a certain (always present)
 * source.  This is the engine behind the rewriter's
 * recursive-reachability route; the returned tokens are ordinary
 * provenance tokens usable with the whole evaluation surface.
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param block_keys per-edge BID key variable (nil UUID = independent
 *        tuple; alternatives sharing a key are mutually exclusive, e.g.
 *        from repair_key)
 * @param block_indices per-edge outcome index within its block
 * @param source_vertices the source vertices
 * @param source_tokens per-source provenance token (nil UUID = certain)
 * @param source_probabilities per-source probability
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_materialize(
  IN sources INT[],
  IN destinations INT[],
  IN tokens UUID[],
  IN probabilities DOUBLE PRECISION[],
  IN block_keys UUID[],
  IN block_indices INT[],
  IN source_vertices INT[],
  IN source_tokens UUID[],
  IN source_probabilities DOUBLE PRECISION[],
  IN directed BOOLEAN,
  OUT vertex INT,
  OUT token UUID)
  RETURNS SETOF record AS
  'provsql','reachability_materialize' LANGUAGE C VOLATILE;


/**
 * @brief Input leaves of a conjunction-shaped provenance token (internal)
 *
 * Descends a token's circuit through the conjunctive gate types
 * (@c times, and the pass-through @c project / @c eq where-provenance
 * wrappers) down to @c input leaves.  Returns the distinct leaves, or
 * NULL when the circuit contains any other gate type (a disjunctive or
 * aggregate shape, which is not a conjunction of independent tuples).
 * Used by the reachability gathering to accept join-defined edges:
 * a derived edge whose token is a pure conjunction of base tuples.
 *
 * @param token the provenance token
 */
CREATE OR REPLACE FUNCTION token_conjunctive_leaves(token uuid)
  RETURNS uuid[] AS
$$
WITH RECURSIVE walk(g) AS (
  SELECT token
  UNION
  SELECT c FROM walk w, unnest(provsql.get_children(w.g)) AS c
  WHERE provsql.get_gate_type(w.g) IN ('times', 'project', 'eq')
)
SELECT CASE WHEN bool_and(provsql.get_gate_type(g)
                          IN ('times', 'project', 'eq', 'input'))
            THEN array_agg(DISTINCT g)
                   FILTER (WHERE provsql.get_gate_type(g) = 'input')
            ELSE NULL END
FROM walk;
$$ LANGUAGE sql STABLE;

/**
 * @brief Gather the edges of a tracked relation in the columnar form
 * expected by reachability_evaluate (internal)
 *
 * Materializes the edge relation with its provenance tokens and
 * probabilities, maps arbitrary vertex values (compared as text) onto
 * dense integer IDs, and checks that every edge tuple carries a base
 * input token (independent tuples): reachability compilation along the
 * data is only correct when the edges are independent events, so views
 * or query results with derived provenance are rejected.
 *
 * @param rel the provenance-tracked edge relation
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param extra_vertices vertex values (as text) that must be part of
 *        the dense ID space even when they touch no edge -- the source
 *        set in particular; their IDs come back in @c extra_ids
 *        (aligned with the input)
 * @param edge_quals optional deterministic filter over the edge
 *        relation's columns (SQL text, deparsed by the rewriter from
 *        the recursive arm's WHERE clause), restricting which edges
 *        participate
 * @param rel_sql deparsed edge subquery to gather from instead of
 *        @p rel (join-defined edges); the tokens are then conjunctions
 *        of base tuples, validated for shape and disjoint supports
 *
 * The @c vertices output maps the dense IDs back to the original
 * vertex values (as text, 1-indexed), for callers that need to label
 * per-vertex results.
 */
CREATE OR REPLACE FUNCTION gather_reachability_edges(
  IN rel regclass,
  IN source_attribute TEXT,
  IN destination_attribute TEXT,
  IN extra_vertices TEXT[],
  IN edge_quals TEXT DEFAULT NULL,
  IN rel_sql TEXT DEFAULT NULL,
  OUT sources INT[],
  OUT destinations INT[],
  OUT tokens UUID[],
  OUT probabilities DOUBLE PRECISION[],
  OUT block_keys UUID[],
  OUT block_indices INT[],
  OUT extra_ids INT[],
  OUT vertices TEXT[])
AS
$$
DECLARE
  tkind text;
  bkey_expr text;
  sel_probs text;
  sel_bkeys text;
  sel_bidx text;
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  -- Consult the per-table characterisation registry (TID / BID / OPAQUE,
  -- maintained by add_provenance / repair_key and the CTAS lineage hook):
  -- a TID relation is certified all-independent-inputs, a BID relation
  -- holds input or mulinput rows with the block structure given by the
  -- registry's key columns.  Derived (OPAQUE), unregistered, or
  -- subquery-defined edges take the fully dynamic per-token path.
  IF rel IS NOT NULL AND rel_sql IS NULL THEN
    tkind := (provsql.get_table_info(rel::oid)).kind;
  END IF;
  IF tkind NOT IN ('tid', 'bid') THEN
    tkind := NULL;
  END IF;
  IF tkind = 'bid' THEN
    SELECT string_agg(quote_ident(a.attname) || '::text', ' || '','' || '
                      ORDER BY k.ord)
      INTO bkey_expr
      FROM unnest((provsql.get_table_info(rel::oid)).block_key)
             WITH ORDINALITY AS k(attnum, ord)
      JOIN pg_attribute a ON a.attrelid = rel AND a.attnum = k.attnum;
    -- An empty registry key means the whole table is one block.
    bkey_expr := coalesce(bkey_expr, quote_literal(''));
  END IF;
  IF tkind IS NOT NULL AND verbosity >= 20 THEN
    -- The function-level client_min_messages = warning (which silences
    -- the CTAS / DROP TABLE chatter) would also swallow this notice;
    -- lift it for the one RAISE.  The function-level SET restores the
    -- caller's value at exit regardless.
    PERFORM set_config('client_min_messages', 'notice', true);
    RAISE NOTICE 'ProvSQL: catalog characterises % as %', rel, upper(tkind);
    PERFORM set_config('client_min_messages', 'warning', true);
  END IF;

  -- Materialize the edges with their tokens; the planner hook resolves
  -- provenance() over the tracked relation, and remove_provenance strips
  -- the automatic provsql column so the later aggregation is plain SQL.
  -- For a BID relation the synthetic per-block key (a v5 UUID over the
  -- registry key columns' values) is computed here, while the columns
  -- are in scope.
  DROP TABLE IF EXISTS provsql_reachability_edges_tmp;
  EXECUTE format(
    'CREATE TEMP TABLE provsql_reachability_edges_tmp AS '
    || 'SELECT %1$I::text AS u, %2$I::text AS v, provsql.provenance() AS token%5$s '
    || 'FROM %3$s WHERE %1$I IS NOT NULL AND %2$I IS NOT NULL%4$s',
    source_attribute, destination_attribute,
    CASE WHEN rel_sql IS NULL THEN rel::text
         ELSE '(' || rel_sql || ') AS provsql_edge_subquery' END,
    CASE WHEN edge_quals IS NULL THEN ''
         ELSE ' AND (' || edge_quals || ')' END,
    CASE WHEN tkind = 'bid'
         THEN ', public.uuid_generate_v5(provsql.uuid_ns_provsql(), '
              || quote_literal('bidblock' || rel::text || ':')
              || ' || ' || bkey_expr || ') AS bkey'
         ELSE ', NULL::uuid AS bkey' END);
  PERFORM provsql.remove_provenance('provsql_reachability_edges_tmp');

  DROP TABLE IF EXISTS provsql_reachability_support_tmp;
  IF tkind IS NULL THEN
    -- Dynamic path: validate the token shapes and, for conjunction-shaped
    -- (join-defined) tokens, the pairwise disjointness of their supports.
    IF EXISTS (SELECT 1 FROM provsql_reachability_edges_tmp
               WHERE provsql.get_gate_type(token) NOT IN ('input', 'mulinput', 'times',
                                                  'project', 'eq')) THEN
      DROP TABLE provsql_reachability_edges_tmp;
      RAISE EXCEPTION 'reachability: the provenance of % must consist of base input, repair_key, or conjunctive join tokens', coalesce(rel::text, 'the edge query');
    END IF;
    CREATE TEMP TABLE provsql_reachability_support_tmp AS
      SELECT t.token, l.leaf
      FROM (SELECT DISTINCT token FROM provsql_reachability_edges_tmp
            WHERE provsql.get_gate_type(token) IN ('times', 'project', 'eq')) t,
           LATERAL unnest(provsql.token_conjunctive_leaves(t.token)) AS l(leaf);
    IF EXISTS (SELECT 1
               FROM (SELECT DISTINCT token FROM provsql_reachability_edges_tmp) t
               WHERE provsql.get_gate_type(t.token) IN ('times', 'project', 'eq')
                 AND provsql.token_conjunctive_leaves(t.token) IS NULL) THEN
      DROP TABLE provsql_reachability_support_tmp;
      DROP TABLE provsql_reachability_edges_tmp;
      RAISE EXCEPTION 'reachability: a join-defined edge token is not a pure conjunction of base tuples';
    END IF;
    IF EXISTS (SELECT 1 FROM (
                 SELECT leaf FROM provsql_reachability_support_tmp
                 UNION ALL
                 SELECT DISTINCT token FROM provsql_reachability_edges_tmp
                 WHERE provsql.get_gate_type(token) = 'input'
               ) all_leaves
               GROUP BY leaf HAVING count(*) > 1) THEN
      DROP TABLE provsql_reachability_support_tmp;
      DROP TABLE provsql_reachability_edges_tmp;
      RAISE EXCEPTION 'reachability: join-defined edges share base tuples (their supports overlap), so they are not independent';
    END IF;
  END IF;

  -- Per-kind classification expressions for the final aggregation: a TID
  -- relation needs no per-row gate introspection at all; a BID relation
  -- one get_gate_type per row (the input/mulinput split), block keys from
  -- the precomputed column-derived key and indices by numbering within
  -- the block; the dynamic path reads the gates.
  IF tkind = 'tid' THEN
    sel_probs := 'coalesce(provsql.get_prob(e.token), 1.0)';
    sel_bkeys := $sql$'00000000-0000-0000-0000-000000000000'::uuid$sql$;
    sel_bidx  := '0';
  ELSIF tkind = 'bid' THEN
    sel_probs := 'coalesce(provsql.get_prob(e.token), 1.0)';
    sel_bkeys := $sql$CASE WHEN provsql.get_gate_type(e.token) = 'mulinput'
                      THEN e.bkey
                      ELSE '00000000-0000-0000-0000-000000000000'::uuid END$sql$;
    sel_bidx  := 'e.bidx';
  ELSE
    sel_probs := $sql$CASE WHEN provsql.get_gate_type(e.token) IN ('times','project','eq')
                      THEN (SELECT CASE WHEN bool_or(coalesce(provsql.get_prob(s.leaf),1.0) = 0)
                                        THEN 0.0
                                        ELSE exp(sum(ln(coalesce(provsql.get_prob(s.leaf),1.0)))) END
                            FROM provsql_reachability_support_tmp s
                            WHERE s.token = e.token)
                      ELSE coalesce(provsql.get_prob(e.token), 1.0) END$sql$;
    sel_bkeys := $sql$CASE WHEN provsql.get_gate_type(e.token) = 'mulinput'
                      THEN (provsql.get_children(e.token))[1]
                      ELSE '00000000-0000-0000-0000-000000000000'::uuid END$sql$;
    sel_bidx  := $sql$CASE WHEN provsql.get_gate_type(e.token) = 'mulinput'
                      THEN (provsql.get_infos(e.token)).info1 ELSE 0 END$sql$;
  END IF;

  EXECUTE format(
    $sql$
    WITH verts AS (
      SELECT u AS x FROM provsql_reachability_edges_tmp
      UNION SELECT v FROM provsql_reachability_edges_tmp
      UNION SELECT unnest($1)),
    ids AS (
      SELECT x, (row_number() OVER (ORDER BY x))::int AS id FROM verts)
    SELECT array_agg(iu.id), array_agg(iv.id),
           array_agg(e.token),
           array_agg(%s),
           array_agg(%s),
           array_agg(%s),
           (SELECT array_agg(i.id ORDER BY ev.ord)
              FROM unnest($1) WITH ORDINALITY AS ev(x, ord)
              JOIN ids i ON i.x = ev.x),
           (SELECT array_agg(x ORDER BY id) FROM ids)
      FROM (SELECT t.*,
                   (row_number() OVER (PARTITION BY t.bkey))::int AS bidx
            FROM provsql_reachability_edges_tmp t) e
      JOIN ids iu ON iu.x = e.u
      JOIN ids iv ON iv.x = e.v
    $sql$, sel_probs, sel_bkeys, sel_bidx)
    INTO sources, destinations, tokens, probabilities, block_keys,
         block_indices, extra_ids, vertices
    USING extra_vertices;

  DROP TABLE provsql_reachability_edges_tmp;
  DROP TABLE IF EXISTS provsql_reachability_support_tmp;
END
-- No SET search_path: the deparsed edge subquery (and the regclass
-- rendering) must resolve against the caller's search_path; the ProvSQL
-- calls above are schema-qualified instead.
$$ LANGUAGE plpgsql SET client_min_messages = warning;


/**
 * @brief Gather a source relation's vertices, tokens and probabilities
 * (internal)
 *
 * For a provenance-tracked source relation, every tuple must carry a
 * base @c input token (a *probabilistic source set*); for an untracked
 * relation the sources are certain and the tokens come back as the nil
 * UUID.  Vertex values are returned as text, for the shared dense-ID
 * mapping of @c gather_reachability_edges().
 *
 * @param rel the source relation
 * @param source_attribute name of the vertex column
 */
CREATE OR REPLACE FUNCTION gather_reachability_sources(
  IN rel regclass,
  IN source_attribute TEXT,
  OUT source_values TEXT[],
  OUT source_tokens UUID[],
  OUT source_probabilities DOUBLE PRECISION[])
AS
$$
DECLARE
  tracked boolean;
  tkind text;
BEGIN
  SELECT EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = rel AND attname = 'provsql'
      AND atttypid = 'uuid'::regtype AND NOT attisdropped)
  INTO tracked;

  -- Registry consultation: a TID source relation is certified
  -- all-base-input, so the per-row gate check can be skipped; a BID one
  -- holds block-correlated tuples, which a probabilistic source set
  -- cannot model -- reject it before gathering anything.
  IF tracked THEN
    tkind := (get_table_info(rel::oid)).kind;
    IF tkind = 'bid' THEN
      RAISE EXCEPTION 'reachability: % is block-independent (repair_key); block-correlated source sets are not supported', rel;
    END IF;
  END IF;

  DROP TABLE IF EXISTS provsql_reachability_sources_tmp;
  IF tracked THEN
    EXECUTE format(
      'CREATE TEMP TABLE provsql_reachability_sources_tmp AS '
      || 'SELECT %1$I::text AS x, provenance() AS token '
      || 'FROM %2$s WHERE %1$I IS NOT NULL',
      source_attribute, rel);
    PERFORM remove_provenance('provsql_reachability_sources_tmp');
    IF tkind IS DISTINCT FROM 'tid'
       AND EXISTS (SELECT 1 FROM provsql_reachability_sources_tmp
                   WHERE get_gate_type(token) <> 'input') THEN
      DROP TABLE provsql_reachability_sources_tmp;
      RAISE EXCEPTION 'reachability: the provenance of % must consist of base input tokens (independent tuples); views or query results are not supported', rel;
    END IF;
    SELECT array_agg(x), array_agg(token),
           array_agg(coalesce(get_prob(token), 1.0))
      INTO source_values, source_tokens, source_probabilities
      FROM provsql_reachability_sources_tmp;
    DROP TABLE provsql_reachability_sources_tmp;
  ELSE
    EXECUTE format(
      'CREATE TEMP TABLE provsql_reachability_sources_tmp AS '
      || 'SELECT DISTINCT %1$I::text AS x FROM %2$s WHERE %1$I IS NOT NULL',
      source_attribute, rel);
    SELECT array_agg(x),
           array_agg('00000000-0000-0000-0000-000000000000'::uuid),
           array_agg(1.0::float8)
      INTO source_values, source_tokens, source_probabilities
      FROM provsql_reachability_sources_tmp;
    DROP TABLE provsql_reachability_sources_tmp;
  END IF;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SET client_min_messages = warning;

/**
 * @brief Fixpoint driver for the recursive reachability shape:
 * decomposition-aligned compilation with fallback to eval_recursive
 *
 * Called (at plan time, over SPI) by the recursive-CTE lowering when
 * @c provsql.boolean_provenance is on and the CTE matches the linear
 * reachability shape over a tracked base edge relation.  Attempts the
 * decomposition-aligned route -- gather the edges, compile every
 * reachable vertex's certified provenance circuit along a tree
 * decomposition of the data graph, materialise them, and fill the
 * working table with one tokenised row per reachable vertex.  On any
 * failure (data treewidth above the cap, per-node state bound, edges
 * that are not independent base tuples...), falls back to the generic
 * @c eval_recursive() fixpoint, preserving its behaviour exactly.
 *
 * @param edge_rel the provenance-tracked edge relation
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param source_value the base arm's constant, as text
 * @param directed if false, each edge can be traversed both ways
 * @param work_name name of the working temp table (the CTE name)
 * @param colnames comma-separated user column names (for the fallback)
 * @param coldef column definitions of the working table
 * @param coltype type of the CTE's single column
 * @param body_sql deparsed CTE body (for the fallback)
 * @param edge_quals optional deterministic filter over edge columns
 *        (deparsed from the recursive arm's WHERE clause)
 * @param source_rel source relation of a multi-source base arm
 *        (@c SELECT col FROM sources), NULL for the constant form;
 *        tracked sources form a probabilistic source set, untracked
 *        ones are certain
 * @param source_rel_attribute the source relation's vertex column
 * @param edge_sql deparsed edge subquery when the recursive arm joins a
 *        derived (join-defined) edge relation instead of a base one;
 *        NULL for the regclass form
 */
CREATE OR REPLACE FUNCTION eval_reachability(
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  work_name text,
  colnames text,
  coldef text,
  coltype text,
  body_sql text,
  edge_quals text DEFAULT NULL,
  source_rel regclass DEFAULT NULL,
  source_rel_attribute text DEFAULT NULL,
  edge_sql text DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  e record;
  sv text[];
  st uuid[];
  sp double precision[];
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
    IF source_rel IS NOT NULL THEN
      -- Multi-source: gather the source relation (probabilistic when
      -- tracked, certain otherwise).
      SELECT g.source_values, g.source_tokens, g.source_probabilities
        INTO sv, st, sp
        FROM provsql.gather_reachability_sources(source_rel,
                                                 source_rel_attribute) g;
      IF sv IS NULL THEN
        sv := ARRAY[]::text[];
        st := ARRAY[]::uuid[];
        sp := ARRAY[]::float8[];
      END IF;
    ELSE
      -- Constant base arm: one certain source.
      sv := ARRAY[source_value];
      st := ARRAY['00000000-0000-0000-0000-000000000000'::uuid];
      sp := ARRAY[1.0::float8];
    END IF;

    e := provsql.gather_reachability_edges(edge_rel, source_attribute,
                                           destination_attribute,
                                           sv, edge_quals, edge_sql);
    IF to_regclass(work_name) IS NOT NULL THEN
      EXECUTE format('DROP TABLE %I', work_name);
    END IF;
    EXECUTE format('CREATE TEMP TABLE %I (%s, provsql uuid)', work_name, coldef);
    EXECUTE format(
      'INSERT INTO %I SELECT ($1::text[])[m.vertex]::%s, m.token '
      || 'FROM provsql.reachability_materialize($2, $3, $4, $5, $6, $7, $8, $9, $10, $11) m',
      work_name, coltype)
      USING e.vertices, e.sources, e.destinations, e.tokens, e.probabilities,
            e.block_keys, e.block_indices, e.extra_ids, st, sp, directed;
    IF verbosity >= 20 THEN
      RAISE NOTICE 'ProvSQL: recursive CTE "%" compiled along a tree decomposition of %',
        work_name, coalesce(edge_rel::text, 'the join-defined edge query');
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      RAISE NOTICE 'ProvSQL: reachability route for "%" fell back to the generic fixpoint (%)',
        work_name, SQLERRM;
    END IF;
    PERFORM provsql.eval_recursive(body_sql, work_name, colnames, coldef);
  END;
END
$$ LANGUAGE plpgsql;



