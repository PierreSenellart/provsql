-- ----------------------------------------------------------------------
-- provsql 1.9.0 -> 1.10.0
--
-- New SQL surface since 1.9.0:
--   * exact two-terminal network reliability on bounded-treewidth data,
--     compiled along a tree decomposition of the data graph:
--     reachability_probability / reachability_compile_stats over a
--     provenance-tracked edge relation, their columnar C-backed forms
--     reachability_evaluate / reachability_compile_stats, and the
--     internal gather_reachability_edges helper.
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
 * as a single bidirectional edge.  Most users should prefer the
 * @c reachability_probability() wrapper over a provenance-tracked edge
 * relation.
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
 * circuit per vertex reachable from @p source in the all-edges-present
 * world, materialises the (shared, linear-size) circuits in the
 * provenance store -- @c plus / @c times gates carrying the d-DNNF
 * certificate, negated edges as @c monus(one, edge) -- and returns one
 * @c (vertex, token) row per such vertex.  This is the engine behind
 * the rewriter's recursive-reachability route; the returned tokens are
 * ordinary provenance tokens usable with the whole evaluation surface.
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param source the vertex reachability starts from
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_materialize(
  IN sources INT[],
  IN destinations INT[],
  IN tokens UUID[],
  IN probabilities DOUBLE PRECISION[],
  IN source INT,
  IN directed BOOLEAN,
  OUT vertex INT,
  OUT token UUID)
  RETURNS SETOF record AS
  'provsql','reachability_materialize' LANGUAGE C VOLATILE;

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
 * @param source_value source vertex, as text
 * @param target_value target vertex, as text
 */
CREATE OR REPLACE FUNCTION gather_reachability_edges(
  IN rel regclass,
  IN source_attribute TEXT,
  IN destination_attribute TEXT,
  IN source_value TEXT,
  IN target_value TEXT,
  OUT sources INT[],
  OUT destinations INT[],
  OUT tokens UUID[],
  OUT probabilities DOUBLE PRECISION[],
  OUT source_id INT,
  OUT target_id INT)
AS
$$
BEGIN
  -- Materialize the edges with their tokens; the planner hook resolves
  -- provenance() over the tracked relation, and remove_provenance strips
  -- the automatic provsql column so the later aggregation is plain SQL.
  DROP TABLE IF EXISTS provsql_reachability_edges_tmp;
  EXECUTE format(
    'CREATE TEMP TABLE provsql_reachability_edges_tmp AS '
    || 'SELECT %1$I::text AS u, %2$I::text AS v, provenance() AS token '
    || 'FROM %3$s WHERE %1$I IS NOT NULL AND %2$I IS NOT NULL',
    source_attribute, destination_attribute, rel);
  PERFORM remove_provenance('provsql_reachability_edges_tmp');

  IF EXISTS (SELECT 1 FROM provsql_reachability_edges_tmp
             WHERE get_gate_type(token) <> 'input') THEN
    DROP TABLE provsql_reachability_edges_tmp;
    RAISE EXCEPTION 'reachability: the provenance of % must consist of base input tokens (independent tuples); views or query results are not supported', rel;
  END IF;

  WITH vertices AS (
    SELECT u AS x FROM provsql_reachability_edges_tmp
    UNION SELECT v FROM provsql_reachability_edges_tmp
    UNION SELECT source_value
    UNION SELECT target_value),
  ids AS (
    SELECT x, (row_number() OVER (ORDER BY x))::int AS id FROM vertices)
  SELECT array_agg(iu.id), array_agg(iv.id),
         array_agg(e.token), array_agg(coalesce(get_prob(e.token), 1.0)),
         (SELECT id FROM ids WHERE x = source_value),
         (SELECT id FROM ids WHERE x = target_value)
    INTO sources, destinations, tokens, probabilities, source_id, target_id
    FROM provsql_reachability_edges_tmp e
    JOIN ids iu ON iu.x = e.u
    JOIN ids iv ON iv.x = e.v;

  DROP TABLE provsql_reachability_edges_tmp;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SET client_min_messages = warning;

/**
 * @brief Exact reachability probability over a provenance-tracked edge
 * relation with bounded-treewidth data
 *
 * Probability that @p target is reachable from @p source through the
 * edges of @p rel (two-terminal network reliability), each edge tuple
 * being an independent event with the probability assigned by
 * @c set_prob().  The computation runs in time linear in the number of
 * edges when the graph has bounded treewidth, by compiling the query
 * along a tree decomposition of the data graph -- including on cyclic
 * graphs, out of reach of the WITH RECURSIVE provenance fixpoint.
 * Fails when the data treewidth exceeds the supported limit.
 *
 * Vertices are compared as text, so any vertex column type works
 * (string literals must be cast explicitly, e.g. 'a'::text, for
 * PostgreSQL's polymorphic-type resolution).  An
 * undirected graph can be given either as a directed relation queried
 * with @p directed = false, or as pairs of mutual-reverse tuples
 * sharing their provenance token.
 *
 * @param rel the provenance-tracked edge relation
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param source the vertex reachability starts from
 * @param target the vertex whose reachability is evaluated
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_probability(
  rel regclass,
  source_attribute TEXT,
  destination_attribute TEXT,
  source ANYELEMENT,
  target ANYELEMENT,
  directed BOOLEAN DEFAULT TRUE)
  RETURNS DOUBLE PRECISION AS
$$
DECLARE
  e record;
BEGIN
  IF source IS NULL OR target IS NULL THEN
    RAISE EXCEPTION 'reachability_probability: source and target must not be NULL';
  END IF;
  e := gather_reachability_edges(rel, source_attribute,
                                 destination_attribute,
                                 source::text, target::text);
  RETURN reachability_evaluate(e.sources, e.destinations, e.tokens,
                               e.probabilities, e.source_id, e.target_id,
                               directed);
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public;

/**
 * @brief Reachability probability plus compilation statistics over a
 * provenance-tracked edge relation
 *
 * Same as @c reachability_probability(), additionally returning the
 * structural statistics of the compilation (data treewidth, number of
 * decomposition bags, maximum dynamic-programming state count, d-DNNF
 * size, number of edge variables); see the columnar
 * @c reachability_compile_stats().
 *
 * @param rel the provenance-tracked edge relation
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param source the vertex reachability starts from
 * @param target the vertex whose reachability is evaluated
 * @param directed if false, each edge can be traversed both ways
 */
CREATE OR REPLACE FUNCTION reachability_compile_stats(
  IN rel regclass,
  IN source_attribute TEXT,
  IN destination_attribute TEXT,
  IN source ANYELEMENT,
  IN target ANYELEMENT,
  IN directed BOOLEAN DEFAULT TRUE,
  OUT probability DOUBLE PRECISION,
  OUT data_treewidth INT,
  OUT nb_bags BIGINT,
  OUT max_states BIGINT,
  OUT nb_gates BIGINT,
  OUT nb_variables BIGINT)
AS
$$
DECLARE
  e record;
BEGIN
  IF source IS NULL OR target IS NULL THEN
    RAISE EXCEPTION 'reachability_compile_stats: source and target must not be NULL';
  END IF;
  e := gather_reachability_edges(rel, source_attribute,
                                 destination_attribute,
                                 source::text, target::text);
  SELECT s.probability, s.data_treewidth, s.nb_bags, s.max_states,
         s.nb_gates, s.nb_variables
    INTO probability, data_treewidth, nb_bags, max_states,
         nb_gates, nb_variables
    FROM reachability_compile_stats(e.sources, e.destinations, e.tokens,
                                    e.probabilities, e.source_id,
                                    e.target_id, directed) AS s;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public;

