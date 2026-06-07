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
--     reachability_materialize / reachability_materialize_hops /
--     reachability_evaluate / reachability_compile_stats columnar
--     internals, and the gather_reachability_edges helper.
--   * a canonical-address probe in provenance_plus, serving the
--     pre-created within-bound gates of the bounded-hop route.
--   * the labelled assumption marker: gate type 'assumed_boolean' is
--     renamed 'assumed' with the assumption kind in the gate's extra
--     label; provenance_assume(token, assumption) is the constructor,
--     assume_boolean(token) kept as a compatibility wrapper.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- The Boolean-assumption marker generalises to a labelled assumption
-- marker: the gate type is renamed 'assumed' (ordinality preserved) and
-- the assumption kind ('boolean' / 'absorptive') lives in the gate's
-- extra label, absent on pre-existing gates and then defaulting to the
-- historical 'boolean'.
ALTER TYPE provenance_gate RENAME VALUE 'assumed_boolean' TO 'assumed';

/**
 * @brief Wrap @p token in a fresh @c gate_assumed carrying @p assumption
 *        as its label, and return the wrapper's UUID.
 *
 * Public primitive callable from any rewrite or driver that needs to
 * flag a sub-circuit as sound only under an evaluation assumption:
 *
 * - @c 'boolean' -- the sub-circuit only preserves the Boolean function
 *   of the lineage (e.g. the safe-query rewrite collapses derivation
 *   multiplicities); transparent for semirings admitting a homomorphism
 *   from Boolean functions.
 * - @c 'absorptive' -- the sub-circuit was truncated at the absorptive
 *   value fixpoint (cyclic recursive query); transparent for absorptive
 *   semirings (probability, boolean, min-plus over nonnegative
 *   costs...), fatal for the rest (counting, why-provenance).
 *
 * Incompatible evaluators raise a @c CircuitException.  Always kept as
 * an explicit node in PROV-XML export.
 *
 * The wrapper UUID is content-derived via @c uuid_generate_v5 on the
 * assumption and the child, so identical children always wrap to the
 * same outer UUID per assumption.  No-op (returns NULL) on a NULL
 * input.
 */
CREATE OR REPLACE FUNCTION provenance_assume(token UUID, assumption TEXT)
  RETURNS UUID AS
$$
DECLARE
  wrapped uuid;
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  IF assumption NOT IN ('boolean', 'absorptive') THEN
    RAISE EXCEPTION 'provenance_assume: unknown assumption %', assumption;
  END IF;
  wrapped := public.uuid_generate_v5(uuid_ns_provsql(),
                                     concat('assumed', assumption, token));
  PERFORM create_gate(wrapped, 'assumed', ARRAY[token]);
  PERFORM set_extra(wrapped, assumption);
  RETURN wrapped;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

/**
 * @brief Wrap @p token in a Boolean-assumption marker (compatibility
 *        name; see @c provenance_assume).
 */
CREATE OR REPLACE FUNCTION assume_boolean(token UUID) RETURNS UUID AS
$$
SELECT provsql.provenance_assume(token, 'boolean');
$$ LANGUAGE sql SECURITY DEFINER PARALLEL SAFE;

-- eval_recursive: cyclic data now stops at the absorptive value fixpoint
-- under the 'absorptive'/'boolean' provenance classes, with the resulting
-- tokens wrapped in the 'absorptive' assumption marker.
/**
 * @brief Driver for provenance over recursive queries (WITH RECURSIVE).
 *
 * Invoked by the planner hook (@c lower_recursive_cte in @c provsql.c) when it
 * lowers a recursive CTE whose body touches provenance-tracked relations.  The
 * hook deparses the CTE body to SQL and calls this function, which runs naive
 * bottom-up (fixpoint) evaluation: each round re-evaluates the body
 * @c base @c UNION @c recursive over a tracked working table until the
 * provenance tokens stop changing.  Every round goes through ProvSQL's normal
 * rewriting, so the recursive join yields @c times gates, the untracked base
 * branch yields @c gate_one, and the @c UNION yields the @c plus merge of
 * alternative derivations -- no provenance is plumbed by hand here.  The result
 * is left in a tracked temp table named @p work_name, which the hook then scans
 * in place of the CTE.
 *
 * The working tables (@p work_name and a scratch @c _new) are created once and
 * reused across rounds (TRUNCATE + INSERT), so the round count never
 * accumulates relation locks.  Because content-addressed gate UUIDs make
 * structurally identical sub-circuits share, the fixpoint test is an exact
 * relational @c EXCEPT and the circuit stays the shared (polynomial) form.
 *
 * Scope: UNION (set) recursion.  On *acyclic* input the structural fixpoint is
 * reached and the resulting circuit is the universal provenance, sound for any
 * semiring.  On *cyclic* input the circuit never stabilises structurally; when
 * the session's provenance class (@c provsql.provenance) is @c 'absorptive' or
 * @c 'boolean' we instead stop at the value-fixpoint bound (number of
 * derivable tuples) -- every minimal, tuple-repetition-free derivation is then
 * covered, and the longer ones are absorbed in any absorptive semiring (after
 * Deutch, Milo, Roy & Tannen, ICDT 2014) -- and wrap the resulting tokens in
 * the @c 'absorptive' assumption marker, so that non-absorptive semiring
 * evaluations (counting, why-provenance: genuinely infinite on cyclic data)
 * refuse them while probability, Boolean, formula-as-circuit and min-plus
 * evaluations proceed.  Under the general classes, cyclic input trips the
 * @p max_iter guard.
 *
 * This function has no @c SET @c search_path on purpose: @p body_sql is the
 * caller's deparsed query and must resolve relation names in the caller's path.
 *
 * @param body_sql   the recursive CTE body, e.g.
 *                   @c 'SELECT 1 UNION SELECT e.dst FROM edge e JOIN reach r ON e.src=r.node'
 * @param work_name  the working relation name @p body_sql references (the CTE name)
 * @param colnames   comma-separated user columns, e.g. @c 'node'
 * @param coldef     column definitions for the working table, e.g. @c 'node integer'
 * @param max_iter   safety bound on fixpoint rounds (non-termination guard)
 */
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
  -- Under an absorptive semiring the provenance *value* converges on cyclic
  -- data even though the circuit keeps growing structurally.  A minimal
  -- derivation cannot repeat a tuple, so it has depth <= (number of derivable
  -- tuples); after that many naive rounds the value equals the least fixpoint,
  -- and the surplus (longer, cyclic) derivations are absorbed at evaluation
  -- time.  We learn that bound from the tuple-set fixpoint, stop there, and
  -- mark the resulting tokens with the 'absorptive' assumption so evaluation
  -- under a non-absorptive semiring refuses rather than silently returning a
  -- truncated value.
  absorptive_mode boolean :=
    coalesce(current_setting('provsql.provenance', true), 'semiring')
      IN ('absorptive', 'boolean');
  truncated boolean := false; -- exited at the value fixpoint (cyclic data)
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

    -- In an absorptive class, learn the round bound from the tuple-set
    -- fixpoint (the set always stabilises after finitely many rounds, even on
    -- cyclic data).
    IF absorptive_mode AND ntuples IS NULL THEN
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

    -- Absorptive class on cyclic data: once the value-fixpoint bound is
    -- reached (plus one confirming round, so that acyclic circuits whose
    -- token depth lags the tuple-set saturation still exit through the
    -- structural test above, untagged) we stop, even though the circuit
    -- is not structurally stable.
    IF absorptive_mode AND ntuples IS NOT NULL AND iters >= ntuples + 1 THEN
      truncated := true;
      EXIT;
    END IF;
  END LOOP;

  -- Tokens of a truncated (cyclic) fixpoint are sound only under absorptive
  -- evaluation: record that in the circuit itself.
  IF truncated THEN
    EXECUTE format(
      'UPDATE %I SET provsql = provsql.provenance_assume(provsql, ''absorptive'')',
      work_name);
  END IF;
END
$$ LANGUAGE plpgsql SET client_min_messages = warning;

-- sr_tropical gains the 'nonnegative' flag (absorptive min-plus,
-- accepting cyclic-recursion tokens); the two-argument form is
-- subsumed by the default.
DROP FUNCTION sr_tropical(ANYELEMENT, regclass);
/** @brief Evaluate provenance over the tropical (min-plus) m-semiring
 *
 * Inputs are read as %float8 cost values; the additive identity
 * is <tt>'Infinity'::%float8</tt> and the multiplicative identity is 0.
 * Returns the cost of the cheapest derivation.
 *
 * With @p nonnegative, input costs are checked nonnegative and the
 * semiring is *absorptive*: evaluation then also accepts circuits
 * carrying the @c 'absorptive' assumption marker -- notably cyclic
 * recursive queries truncated at the absorptive value fixpoint, giving
 * exact min-cost reachability on cyclic data.
 */
CREATE FUNCTION sr_tropical(token ANYELEMENT, token2value regclass,
                            nonnegative BOOLEAN = false)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    CASE WHEN nonnegative THEN 'tropical_nonneg' ELSE 'tropical' END,
    0::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/**
 * @brief Create a plus (sum) gate from an array of provenance tokens
 *
 * Filters out NULL and zero-gates; returns gate_zero() if all tokens
 * are trivial, or a single token if only one remains.  Before creating
 * a gate, probes the *canonical* address of the multiset -- a dedicated
 * v5 recipe namespace over the sorted tokens (plus is commutative), in
 * which this function never creates anything, so a gate found there is
 * always a deliberate pre-creation computing the same sum.  That is the
 * bounded-hop reachability route's hook: it plants, at the canonical
 * address of a vertex's per-length tokens, a certified gate over its
 * native within-bound circuit, keeping the natural hop-discarding query
 * on the linear evaluation route.  Absent a canonical gate, the
 * historical order-dependent recipe is used unchanged, so ordinary plus
 * gates (and their formula rendering) are untouched.
 */
CREATE OR REPLACE FUNCTION provenance_plus(tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  SELECT array_agg(t) FROM unnest(tokens) t
  WHERE t IS NOT NULL AND t <> gate_zero()
  INTO filtered_tokens;

  c:=array_length(filtered_tokens, 1);

  IF c = 0 THEN
    plus_token := gate_zero();
  ELSIF c = 1 THEN
    plus_token := filtered_tokens[1];
  ELSE
    -- Computed separately from the filtering aggregate above: an ORDER
    -- BY aggregate there would make the planner feed *both* aggregates
    -- sorted input, scrambling the stored (aggregation-order) children.
    SELECT uuid_generate_v5(uuid_ns_provsql(),
                            concat('plus-canonical', array_agg(t ORDER BY t)))
    FROM unnest(filtered_tokens) t
    INTO canonical;
    IF get_gate_type(canonical) = 'plus' THEN
      -- A deliberate pre-creation at the canonical address: same
      -- children, same sum.
      plus_token := canonical;
    ELSE
      plus_token := uuid_generate_v5(
        uuid_ns_provsql(),
        concat('plus', filtered_tokens));

      PERFORM create_gate(plus_token, 'plus', filtered_tokens);
    END IF;
  END IF;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

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
 * @brief Bounded-hop variant of @c reachability_materialize() (internal)
 *
 * Compiles, along a tree decomposition of the data graph, one certified
 * provenance circuit per (vertex, walk length) pair achievable within
 * @p hop_bound edges -- the rows a hop-counting recursive CTE derives,
 * row @c (v,h) meaning "some *walk* of exactly @c h edges connects a
 * present source to @c v" -- and returns them as @c (vertex, hops,
 * token) with @p hop_seed added to the lengths (the CTE base arm's hop
 * constant).  Also pre-creates, per vertex, the certified gate that a
 * hop-discarding query's deduplication will address, wired to the
 * compilation's native within-bound root, so the natural "within k
 * hops" probability evaluates through the linear certified route.
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param block_keys per-edge BID key variable (nil UUID = independent)
 * @param block_indices per-edge outcome index within its block
 * @param source_vertices the source vertices
 * @param source_tokens per-source provenance token (nil UUID = certain)
 * @param source_probabilities per-source probability
 * @param directed if false, each edge can be traversed both ways
 * @param hop_bound maximum walk length
 * @param hop_seed hop value of the base arm (added to reported lengths)
 */
CREATE OR REPLACE FUNCTION reachability_materialize_hops(
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
  IN hop_bound INT,
  IN hop_seed INT,
  OUT vertex INT,
  OUT hops INT,
  OUT token UUID)
  RETURNS SETOF record AS
  'provsql','reachability_materialize_hops' LANGUAGE C VOLATILE;


/**
 * @brief Per-group "some member reachable" compilation (columnar form,
 * internal)
 *
 * For each distinct group in the parallel @p group_ids /
 * @p member_vertices arrays, compiles the certified circuit of "some
 * member vertex is reachable from a present source" along the data
 * decomposition -- the disjunction over the group's *correlated*
 * per-vertex reachability events, deterministic by construction
 * through the set-reachability state bit -- materialises it, and
 * returns one @c (group_id, token) row per group.  Engine behind the
 * rewriter's cross-vertex aggregation planting.
 *
 * @param sources source vertex of each edge (dense integer IDs)
 * @param destinations destination vertex of each edge
 * @param tokens provenance token of each edge tuple
 * @param probabilities probability of each edge tuple
 * @param block_keys per-edge BID key variable (nil UUID = independent)
 * @param block_indices per-edge outcome index within its block
 * @param source_vertices the source vertices
 * @param source_tokens per-source provenance token (nil UUID = certain)
 * @param source_probabilities per-source probability
 * @param directed if false, each edge can be traversed both ways
 * @param group_ids group identifier of each member row
 * @param member_vertices member vertex of each member row
 */
CREATE OR REPLACE FUNCTION reachability_materialize_any(
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
  IN group_ids INT[],
  IN member_vertices INT[],
  OUT group_id INT,
  OUT token UUID)
  RETURNS SETOF record AS
  'provsql','reachability_materialize_any' LANGUAGE C VOLATILE;

/**
 * @brief Plant certified any-member-reachable gates for a grouped
 * reachability aggregation (internal)
 *
 * Called (at plan time, over SPI) by the recursive-CTE lowering when
 * the outer query aggregates a reachability working table by a column
 * of a joined, untracked member relation: @c GROUP @c BY collapses
 * each group's per-vertex reach tokens with @c provenance_plus, whose
 * disjuncts are correlated (they share edges) and would otherwise
 * leave the certified route.  For each multi-member group this
 * pre-creates, at the canonical address of the group's token multiset,
 * a certified single-child plus over the group's native
 * any-member-reachable circuit (@c reachability_materialize_any), so
 * the natural aggregation stays on the linear evaluation route.
 * Best-effort: any failure leaves the generic path untouched (notice
 * under verbosity 10).
 *
 * @param work_name the lowered CTE's working table
 * @param node_attribute its vertex column
 * @param member_rel the joined member relation (must be untracked)
 * @param member_attribute the member relation's join column
 * @param group_attribute the member relation's grouping column
 * @param edge_rel the tracked edge relation (as for eval_reachability)
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param source_value the base arm's constant, as text
 * @param directed if false, each edge can be traversed both ways
 * @param edge_quals optional deterministic filter over edge columns
 * @param source_rel source relation of a multi-source base arm
 * @param source_rel_attribute the source relation's vertex column
 * @param edge_sql deparsed edge subquery (join-defined edges)
 */
CREATE OR REPLACE FUNCTION plant_reach_any_groups(
  work_name text,
  node_attribute text,
  member_rel regclass,
  member_attribute text,
  group_attribute text,
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  edge_quals text DEFAULT NULL,
  source_rel regclass DEFAULT NULL,
  source_rel_attribute text DEFAULT NULL,
  edge_sql text DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  e record;
  grp record;
  m record;
  sv text[];
  st uuid[];
  sp double precision[];
  gids int[] := ARRAY[]::int[];
  mids int[] := ARRAY[]::int[];
  vid int;
  canonical uuid;
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
    -- A tracked member relation would make the aggregated tokens
    -- per-row products, not the bare reach tokens: nothing to plant.
    IF EXISTS (SELECT 1 FROM pg_attribute
               WHERE attrelid = member_rel AND attname = 'provsql'
                 AND atttypid = 'uuid'::regtype AND NOT attisdropped) THEN
      RETURN;
    END IF;

    IF source_rel IS NOT NULL THEN
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
      sv := ARRAY[source_value];
      st := ARRAY['00000000-0000-0000-0000-000000000000'::uuid];
      sp := ARRAY[1.0::float8];
    END IF;

    e := provsql.gather_reachability_edges(edge_rel, source_attribute,
                                           destination_attribute,
                                           sv, edge_quals, edge_sql);

    -- The groups, replicating the user's join semantics: per group, the
    -- member vertices and the multiset of their reach tokens (with the
    -- multiplicity the join produces).  Single-member groups need no
    -- planting (provenance_plus passes a single token through).
    -- Two steps: materialise the joined rows with their per-row tokens
    -- (tracked CTAS, then strip the automatic provsql column), and only
    -- then aggregate the now-plain table -- aggregating provenance()
    -- inside a grouped tracked query would be rewritten as a
    -- provenance-aware aggregation, which is not what the planting
    -- needs.
    DROP TABLE IF EXISTS provsql_reach_any_flat_tmp;
    EXECUTE format(
      'CREATE TEMP TABLE provsql_reach_any_flat_tmp AS '
      || 'SELECT w.%1$I::text AS node_val, provsql.provenance() AS tok, '
      || '       t.%5$I AS grp_key '
      || 'FROM %2$I w JOIN %3$s t ON w.%1$I = t.%4$I',
      node_attribute, work_name, member_rel::text, member_attribute,
      group_attribute);
    PERFORM provsql.remove_provenance('provsql_reach_any_flat_tmp');
    DROP TABLE IF EXISTS provsql_reach_any_groups_tmp;
    CREATE TEMP TABLE provsql_reach_any_groups_tmp AS
      SELECT (row_number() OVER ())::int AS gid, members, toks FROM (
        SELECT array_agg(node_val) AS members, array_agg(tok) AS toks
        FROM provsql_reach_any_flat_tmp
        GROUP BY grp_key HAVING count(*) >= 2) g;
    DROP TABLE provsql_reach_any_flat_tmp;

    FOR grp IN SELECT gid, members FROM provsql_reach_any_groups_tmp LOOP
      FOR m IN SELECT DISTINCT unnest(grp.members) AS val LOOP
        vid := array_position(e.vertices, m.val);
        IF vid IS NOT NULL THEN
          gids := gids || grp.gid;
          mids := mids || vid;
        END IF;
      END LOOP;
    END LOOP;
    IF cardinality(gids) = 0 THEN
      DROP TABLE provsql_reach_any_groups_tmp;
      RETURN;
    END IF;

    FOR grp IN
      SELECT a.group_id, a.token AS any_token, t.toks
      FROM provsql.reachability_materialize_any(
             e.sources, e.destinations, e.tokens, e.probabilities,
             e.block_keys, e.block_indices, e.extra_ids, st, sp,
             directed, gids, mids) a
      JOIN provsql_reach_any_groups_tmp t ON t.gid = a.group_id
    LOOP
      canonical := public.uuid_generate_v5(
        provsql.uuid_ns_provsql(),
        concat('plus-canonical',
               (SELECT array_agg(tok ORDER BY tok)
                FROM unnest(grp.toks) tok)));
      PERFORM provsql.create_gate(canonical, 'plus', ARRAY[grp.any_token]);
      PERFORM provsql.set_infos(canonical, 1);
    END LOOP;
    DROP TABLE provsql_reach_any_groups_tmp;
    IF verbosity >= 20 THEN
      -- Lift the function-level client_min_messages = warning for the
      -- one RAISE; the function-level SET restores the caller's value.
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: certified any-member gates planted for the aggregation of "%" by %.%',
        work_name, member_rel, group_attribute;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: any-member planting for "%" skipped (%)',
        work_name, SQLERRM;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  END;
END
-- No SET search_path: the deparsed edge subquery must resolve against
-- the caller's path; ProvSQL internals are schema-qualified.
$$ LANGUAGE plpgsql SET client_min_messages = warning;

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
 * @param hop_bound maximum number of recursive steps for the
 *        hop-counting CTE shape (NULL for plain reachability)
 * @param hop_seed the base arm's hop constant (hop-counting shape)
 * @param hops_position 1-based position of the hop column among the
 *        CTE's two columns (hop-counting shape)
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
  edge_sql text DEFAULT NULL,
  hop_bound int DEFAULT NULL,
  hop_seed int DEFAULT NULL,
  hops_position int DEFAULT NULL)
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
    IF hop_bound IS NULL THEN
      EXECUTE format(
        'INSERT INTO %I SELECT ($1::text[])[m.vertex]::%s, m.token '
        || 'FROM provsql.reachability_materialize($2, $3, $4, $5, $6, $7, $8, $9, $10, $11) m',
        work_name, coltype)
        USING e.vertices, e.sources, e.destinations, e.tokens, e.probabilities,
              e.block_keys, e.block_indices, e.extra_ids, st, sp, directed;
    ELSE
      -- Hop-counting shape: one row per (vertex, walk length), the hop
      -- column in its CTE position.
      EXECUTE format(
        'INSERT INTO %I SELECT %s, m.token '
        || 'FROM provsql.reachability_materialize_hops($2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) m',
        work_name,
        CASE WHEN hops_position = 1
             THEN format('m.hops, ($1::text[])[m.vertex]::%s', coltype)
             ELSE format('($1::text[])[m.vertex]::%s, m.hops', coltype) END)
        USING e.vertices, e.sources, e.destinations, e.tokens, e.probabilities,
              e.block_keys, e.block_indices, e.extra_ids, st, sp, directed,
              hop_bound, hop_seed;
    END IF;
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



