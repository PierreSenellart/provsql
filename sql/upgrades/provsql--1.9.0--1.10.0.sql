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
--     pre-created within-bound gates of the bounded-hop route, and its
--     provenance_times twin, serving the pre-created
--     all-members-reachable gates of self-join conjunctions
--     (plant_reach_cover / reachability_materialize_cover).
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

-- Conditioning marker for the | / cond operator (see the conditioning
-- block at the end of this script): a three-child gate evaluated only in
-- the measure interpretation.  ADD VALUE is not used in this transaction
-- (the cond function below only references it as a literal / in
-- create_gate, never materialising the value), so it is upgrade-safe.
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'conditioned';

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
 * query under the 'absorptive' (or 'boolean') provenance class, which
 * the query rewriter compiles through @c eval_reachability() /
 * @c reachability_materialize().
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
 * provenance tokens usable with the whole evaluation surface, wrapped
 * in the 'absorptive' assumption marker (the compiled circuit is the
 * exact Boolean lineage but only the absorptive quotient of the
 * infinite recursive semiring provenance: probability and absorptive
 * semiring evaluations -- e.g. nonnegative min-plus -- are exact,
 * counting and why-provenance refuse).
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
 * @param member_quals optional deterministic filter over the member
 *        relation's columns (table-qualified as @c t.<col>), restricting
 *        which members participate in each group
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
  edge_sql text DEFAULT NULL,
  member_quals text DEFAULT NULL)
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
      || 'FROM %2$I w JOIN %3$s t ON w.%1$I = t.%4$I'
      -- The member-relation filter restricts which members participate
      -- (deparsed table-qualified as t.<col>); the working table side
      -- carries no provenance distinction here.
      || coalesce(' WHERE ' || member_quals, ''),
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
 * the provenance class is 'absorptive' or 'boolean'
 * (@c provsql.provenance) and the CTE matches the linear
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

/**
 * @brief Create a times (product) gate from multiple provenance tokens
 *
 * Filters out NULL and one-gates; returns gate_one() if all tokens
 * are trivial, or a single token if only one remains.
 *
 * Before creating an ordinary gate, the *times-canonical* address of
 * the surviving multiset -- @c uuid5('times-canonical{sorted tokens}')
 * -- is probed: the reachability rewriter pre-creates there, for
 * self-join conjunctions of reachability tokens, a certified
 * equivalent (the all-members-reachable circuit; see
 * @c plant_reach_cover).  Ordinary creation never writes under that
 * recipe, so a hit is always a deliberate plant; the historical
 * order-dependent recipe is used unchanged otherwise, so ordinary
 * times gates (and their formula rendering) are untouched.
 */
CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  -- Dispatch on the FILTERED count: a single survivor short-circuits
  -- to that token directly (no useless single-child times gate); zero
  -- survivors collapse to the identity. Using array_length(tokens, 1)
  -- here would miss the [one, cmp] → [cmp] case, leaving the cmp wrapped
  -- in a one-child times when its only sibling was gate_one().
  CASE coalesce(array_length(filtered_tokens, 1), 0)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=filtered_tokens[1];
    ELSE
      -- Computed separately from the filtering aggregate above: an
      -- ORDER BY aggregate there would make the planner feed *both*
      -- aggregates sorted input, scrambling the stored children order.
      SELECT uuid_generate_v5(uuid_ns_provsql(),
                              concat('times-canonical', array_agg(t ORDER BY t)))
      FROM unnest(filtered_tokens) t
      INTO canonical;
      IF get_gate_type(canonical) = 'times' THEN
        -- A deliberate pre-creation at the canonical address: same
        -- children, same product.
        times_token := canonical;
      ELSE
        times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

        PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
      END IF;
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

/**
 * @brief Compile and materialise the "every member vertex reachable"
 * (k-terminal / coverage) circuit (columnar form, internal)
 *
 * Arguments as @c reachability_materialize_any() with a single member
 * set: compiles the certified circuit of "every member vertex is
 * reachable from a present source" -- the conjunction over the
 * members' *correlated* per-vertex events, deterministic by
 * construction through the pending rescuer-set congruence --
 * materialises it, and returns its token, wrapped in the
 * @c 'absorptive' assumption marker.  Probability evaluation gives the
 * k-terminal reliability; nonnegative min-plus the cost of the
 * cheapest covering subgraph (directed Steiner cost), shared edges
 * paid once.  A member vertex absent from the graph is unreachable:
 * the circuit is then constant false.
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
 * @param member_vertices the member vertices (dense IDs)
 */
CREATE OR REPLACE FUNCTION reachability_materialize_cover(
  sources INT[],
  destinations INT[],
  tokens UUID[],
  probabilities DOUBLE PRECISION[],
  block_keys UUID[],
  block_indices INT[],
  source_vertices INT[],
  source_tokens UUID[],
  source_probabilities DOUBLE PRECISION[],
  directed BOOLEAN,
  member_vertices INT[])
  RETURNS UUID AS
  'provsql','reachability_materialize_cover' LANGUAGE C VOLATILE;

/**
 * @brief Plant the certified all-members-reachable gate for a
 * reachability self-join conjunction (internal)
 *
 * Called (at plan time, over SPI) by the recursive-CTE lowering when
 * the outer query self-joins a reachability working table with one
 * constant node binding per reference -- "are these k vertices all
 * reachable" -- whose row provenance @c provenance_times() computes as
 * the product of *correlated* per-vertex reach tokens (they share
 * edges).  This pre-creates, at the times-canonical address of that
 * token multiset, a certified single-child times over the native
 * all-members-reachable circuit (@c reachability_materialize_cover),
 * so the natural conjunction stays on the linear certified route --
 * with the joint-worlds semantics: probability evaluation gives the
 * k-terminal reliability, and nonnegative min-plus the cost of the
 * cheapest covering subgraph (directed Steiner cost), shared edges
 * paid once where the raw product would pay them once per factor.
 * Best-effort: any failure leaves the generic path untouched (notice
 * under verbosity 10).
 *
 * @param work_name the lowered CTE's working table
 * @param node_attribute its vertex column
 * @param edge_rel the tracked edge relation (as for eval_reachability)
 * @param source_attribute name of the source-vertex column
 * @param destination_attribute name of the destination-vertex column
 * @param source_value the base arm's constant, as text
 * @param directed if false, each edge can be traversed both ways
 * @param node_values the constant node bindings, as text (multiset:
 *        one per self-join reference)
 * @param edge_quals optional deterministic filter over edge columns
 * @param source_rel source relation of a multi-source base arm
 * @param source_rel_attribute the source relation's vertex column
 * @param edge_sql deparsed edge subquery (join-defined edges)
 */
CREATE OR REPLACE FUNCTION plant_reach_cover(
  work_name text,
  node_attribute text,
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  node_values text[],
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
  val text;
  vid int;
  vids int[] := ARRAY[]::int[];
  tok uuid;
  toks uuid[] := ARRAY[]::uuid[];
  cover_token uuid;
  canonical uuid;
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
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

    -- The bound vertices and their per-row reach tokens, with the
    -- multiplicity the self-join produces.  A vertex absent from the
    -- graph, or from the working table, means the join is empty: no
    -- row will exist, nothing to plant.
    FOREACH val IN ARRAY node_values LOOP
      vid := array_position(e.vertices, val);
      IF vid IS NULL THEN
        RETURN;
      END IF;
      vids := vids || vid;
      EXECUTE format('SELECT provsql FROM %I WHERE %I::text = $1',
                     work_name, node_attribute)
        INTO tok USING val;
      IF tok IS NULL THEN
        RETURN;
      END IF;
      toks := toks || tok;
    END LOOP;

    cover_token := provsql.reachability_materialize_cover(
      e.sources, e.destinations, e.tokens, e.probabilities,
      e.block_keys, e.block_indices, e.extra_ids, st, sp,
      directed, vids);

    SELECT public.uuid_generate_v5(
             provsql.uuid_ns_provsql(),
             concat('times-canonical', array_agg(t ORDER BY t)))
    FROM unnest(toks) t
    INTO canonical;
    PERFORM provsql.create_gate(canonical, 'times', ARRAY[cover_token]);
    PERFORM provsql.set_infos(canonical, 1);
    IF verbosity >= 20 THEN
      -- Lift the function-level client_min_messages = warning for the
      -- one RAISE; the function-level SET restores the caller's value.
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: certified all-members gate planted for the self-join of "%"',
        work_name;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: all-members planting for "%" skipped (%)',
        work_name, SQLERRM;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  END;
END
-- No SET search_path: the deparsed edge subquery must resolve against
-- the caller's path; ProvSQL internals are schema-qualified.
$$ LANGUAGE plpgsql SET client_min_messages = warning;

-- ----------------------------------------------------------------------
-- Conditioning: the | / cond operator (uuid carrier).
--
-- cond(target, evidence) builds the terminal gate_conditioned that the
-- measure evaluators read as P(target ∧ evidence) / P(evidence).  The gate
-- stores [target, evidence, joint] with joint = times(target, evidence), so
-- probability_evaluate is the plain ratio P(joint)/P(evidence); content-
-- addressing makes shared base tuples the same input gate in both circuits,
-- so the conditional is exact and correlation-aware.  Nested conditioning
-- folds (sequential Bayesian update (X|A)|B = X|(A∧B)); the token is
-- terminal (refused as a child of any semiring gate, and by every general
-- sr_* semiring at evaluation).
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION cond(target UUID, evidence UUID) RETURNS UUID AS
$$
DECLARE
  tgt uuid;
  ev  uuid;
  jnt uuid;
  result uuid;
  ch uuid[];
BEGIN
  IF evidence IS NULL OR evidence = gate_one() THEN
    RETURN target;
  END IF;

  tgt := coalesce(target, gate_one());

  IF get_gate_type(tgt) = 'conditioned' THEN
    ch  := get_children(tgt);
    tgt := ch[1];
    ev  := provenance_times(ch[2], evidence);
    jnt := provenance_times(ch[3], evidence);
  ELSE
    ev  := evidence;
    jnt := provenance_times(tgt, evidence);
  END IF;

  result := public.uuid_generate_v5(uuid_ns_provsql(),
                                    concat('conditioned', tgt, ev, jnt));
  PERFORM create_gate(result, 'conditioned', ARRAY[tgt, ev, jnt]);
  RETURN result;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

CREATE OPERATOR | (LEFTARG=UUID, RIGHTARG=UUID, PROCEDURE=cond);

-- Whole-tuple output conditioning marker: given(c) / prefix | c.  A consumed
-- select-list term the rewriter strips, conditioning each output row's
-- provenance on c.  Identity at runtime when the rewriter is inactive.
CREATE OR REPLACE FUNCTION given(evidence UUID) RETURNS UUID AS
$$
  SELECT evidence;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR | (RIGHTARG=UUID, PROCEDURE=given);

-- ----------------------------------------------------------------------
-- Conditioning: the X | C operator for the random_variable carrier.
--
-- random_variable_cond builds a composable two-child gate_conditioned
-- [target, condition]; the moment / support dispatchers below unpack it
-- (rv_conditioned_target / rv_conditioned_prov) and route through the
-- existing conditional evaluator (rv_moment / rv_support).
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION random_variable_cond(rv random_variable, cond uuid)
  RETURNS random_variable AS
$$
DECLARE
  tgt uuid;
  ev  uuid;
  result uuid;
  ch uuid[];
BEGIN
  IF cond IS NULL OR cond = gate_one() THEN
    RETURN rv;
  END IF;

  tgt := (rv)::uuid;
  IF get_gate_type(tgt) = 'conditioned'
     AND array_length(get_children(tgt), 1) = 2 THEN
    -- Fold (X|A)|B = X|(A∧B): the rv-carrier conditioned gate is the
    -- two-child [target, condition] shape; accumulate the new event.
    ch  := get_children(tgt);
    tgt := ch[1];
    ev  := provenance_times(ch[2], cond);
  ELSE
    ev := cond;
  END IF;

  result := public.uuid_generate_v5(uuid_ns_provsql(),
                                    concat('conditioned', tgt, ev));
  PERFORM create_gate(result, 'conditioned', ARRAY[tgt, ev]);
  RETURN (result)::random_variable;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

CREATE OPERATOR | (
  LEFTARG   = random_variable,
  RIGHTARG  = uuid,
  PROCEDURE = random_variable_cond
);

/**
 * @brief Unpack the target of a random-variable conditioning gate.
 *
 * For a two-child @c gate_conditioned @c [target, condition] (the @c "X | C"
 * shape) returns @p target; for any other token returns it unchanged.  Used
 * by the moment / support dispatchers to route a conditioned distribution
 * through the existing conditional evaluator.
 */
CREATE OR REPLACE FUNCTION rv_conditioned_target(token uuid) RETURNS uuid AS
$$
  SELECT CASE
    WHEN provsql.get_gate_type(token) = 'conditioned'
         AND array_length(provsql.get_children(token), 1) = 2
    THEN (provsql.get_children(token))[1]
    ELSE token
  END;
$$ LANGUAGE sql STABLE PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/**
 * @brief Combine a conditioning gate's event with an explicit @p prov.
 *
 * For a two-child @c gate_conditioned @c [target, condition] returns
 * @c "condition ∧ prov"; otherwise returns @p prov unchanged.  Lets a stored
 * @c "X | C" be queried as @c expected(X|C) (prov defaulting to one) or have
 * an extra condition conjoined as @c expected(X|C, extra_prov).
 */
CREATE OR REPLACE FUNCTION rv_conditioned_prov(token uuid, prov uuid)
  RETURNS uuid AS
$$
  SELECT CASE
    WHEN provsql.get_gate_type(token) = 'conditioned'
         AND array_length(provsql.get_children(token), 1) = 2
    THEN provsql.provenance_times((provsql.get_children(token))[2], prov)
    ELSE prov
  END;
$$ LANGUAGE sql STABLE PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION variance(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  m1 float8;
  m2 float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    -- Conditioning on prov is handled inside rv_moment: when prov
    -- resolves to gate_one() (the default, or load-time
    -- simplification of any always-true sub-circuit) the
    -- unconditional analytical path runs unchanged; otherwise the
    -- joint-circuit loader unifies shared gate_rv leaves between
    -- input and prov, and the conditional path runs either
    -- truncated-distribution closed form or MC rejection.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), 2, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    m1 := agg_raw_moment(input::agg_token, 1, prov, method, arguments);
    m2 := agg_raw_moment(input::agg_token, 2, prov, method, arguments);
    IF m1 IS NULL OR m2 IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN m2 - m1 * m1;
  END IF;

  RAISE EXCEPTION 'variance() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, false,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(input::agg_token, k, prov, method, arguments);
  END IF;

  RAISE EXCEPTION 'moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION support(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL,
  OUT lo float8,
  OUT hi float8)
  AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  values_arr float8[];
  total_probability float8;
BEGIN
  IF input IS NULL THEN
    lo := NULL; hi := NULL; RETURN;
  END IF;

  -- Plain numeric: degenerate point support.  Lets `support(2.5)` /
  -- `support(42)` / etc.  return (2.5, 2.5) without making the user
  -- wrap in `as_random`.
  IF pg_typeof(input) IN (
       'smallint'::regtype, 'integer'::regtype, 'bigint'::regtype,
       'numeric'::regtype, 'real'::regtype, 'double precision'::regtype) THEN
    lo := input::double precision;
    hi := input::double precision;
    RETURN;
  END IF;

  -- random_variable is binary-coercible to uuid (explicit cast
  -- below), so a single rv_support call covers both shapes.
  -- rv_support handles
  -- gate_value (point), gate_rv (distribution), gate_arith
  -- (propagated), and falls back to the conservative all-real
  -- interval for any other gate kind.  Conditioning on prov is not
  -- supported (would require restricting the underlying joint
  -- distribution by the indicator of prov, which has no closed form
  -- for the basic distributions we ship).
  IF pg_typeof(input) IN ('random_variable'::regtype, 'uuid'::regtype) THEN
    -- Conditional support: rv_support folds the AND-conjunct interval
    -- constraints from prov into the unconditional support.  When
    -- prov is gate_one() the unconditional support is returned
    -- unchanged.
    SELECT r.lo, r.hi INTO lo, hi
      FROM provsql.rv_support(
             rv_conditioned_target(input::uuid),
             rv_conditioned_prov(input::uuid, prov)) r;
    RETURN;
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF get_gate_type(input::agg_token) <> 'agg' THEN
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for support computation';
    END IF;
    SELECT pp.proname::varchar FROM pg_proc pp
      WHERE oid=(get_infos(input::agg_token)).info1
      INTO aggregation_function;
    child_pairs := get_children(input::agg_token);

    IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
      -- count(col) is a SUM of per-row 0/1 indicators (empty group = 0), so its
      -- support is computed like SUM; count(*) arrives as 'sum'.
      -- Empty agg_token: SUM is identically 0.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := 0; hi := 0; RETURN;
      END IF;
      SELECT sum(LEAST(v, 0::float8)), sum(GREATEST(v, 0::float8))
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM unnest(child_pairs) AS c) sub;
    ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
      -- MIN/MAX over the empty input world are NULL, not ±Infinity (matching the
      -- moment surface): the empty world carries no value, so the support is just
      -- the range of the per-row values [min(v), max(v)].  A structurally empty
      -- aggregate has no defined value at all -> NULL support.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := NULL; hi := NULL; RETURN;
      END IF;

      SELECT min(v), max(v)
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM UNNEST(child_pairs) AS c) sub;
    ELSE
      RAISE EXCEPTION USING MESSAGE=
        'Cannot compute support for aggregation function ' || aggregation_function;
    END IF;
    RETURN;
  END IF;

  RAISE EXCEPTION 'support() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION central_moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  mu float8;
  total float8;
  i integer;
  raw_i float8;
  binom float8;
  -- iterative binomial coefficient C(k, i)
  k_double float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k < 0 THEN
      RAISE EXCEPTION 'central_moment(): k must be non-negative (got %)', k;
    END IF;
    IF k = 0 THEN RETURN 1; END IF;
    IF k = 1 THEN RETURN 0; END IF;

    mu := agg_raw_moment(input::agg_token, 1, prov, method, arguments);
    IF mu IS NULL THEN RETURN NULL; END IF;
    -- mu may be ±Infinity for empty MIN / MAX with positive empty
    -- probability; central_moment is undefined in that case.
    IF mu = 'Infinity'::float8 OR mu = '-Infinity'::float8 THEN
      RETURN mu;
    END IF;

    total := 0;
    binom := 1;  -- C(k, 0)
    k_double := k;
    FOR i IN 0..k LOOP
      raw_i := agg_raw_moment(input::agg_token, i, prov, method, arguments);
      IF raw_i IS NULL THEN RETURN NULL; END IF;
      total := total + binom * power(-mu, k - i) * raw_i;
      -- C(k, i+1) = C(k, i) * (k - i) / (i + 1)
      IF i < k THEN
        binom := binom * (k_double - i) / (i + 1);
      END IF;
    END LOOP;
    RETURN total;
  END IF;

  RAISE EXCEPTION 'central_moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- ----------------------------------------------------------------------
-- Conditioning: the SUM(x) | C operator for the agg_token carrier, and the
-- final moment / support dispatchers carrying both the random_variable and
-- agg_token conditioned-distribution unpacking.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION agg_token_cond(a agg_token, cond uuid)
  RETURNS agg_token AS
$$
DECLARE
  tok uuid;
  ev  uuid;
  result uuid;
  ch uuid[];
BEGIN
  IF cond IS NULL OR cond = gate_one() THEN
    RETURN a;
  END IF;

  tok := (a)::uuid;
  IF get_gate_type(tok) = 'conditioned'
     AND array_length(get_children(tok), 1) = 2 THEN
    ch  := get_children(tok);
    tok := ch[1];
    ev  := provenance_times(ch[2], cond);
  ELSE
    ev := cond;
  END IF;

  result := public.uuid_generate_v5(uuid_ns_provsql(),
                                    concat('conditioned', tok, ev));
  PERFORM create_gate(result, 'conditioned', ARRAY[tok, ev]);
  RETURN agg_token_make(result, agg_token_value(a));
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

CREATE OPERATOR | (
  LEFTARG   = agg_token,
  RIGHTARG  = uuid,
  PROCEDURE = agg_token_cond
);

/**
 * @brief Unpack the target of a conditioned @c agg_token.
 *
 * For a @c "SUM(x) | C" whose provenance token is the two-child
 * @c gate_conditioned @c [agg_target, condition] returns the agg_token over
 * @c agg_target (same running value); for any other agg_token returns it
 * unchanged.  The conditioning event itself is recovered separately via
 * @c rv_conditioned_prov on the token's uuid.
 */
CREATE OR REPLACE FUNCTION agg_conditioned_target(a agg_token)
  RETURNS agg_token AS
$$
  SELECT CASE
    WHEN provsql.get_gate_type((a)::uuid) = 'conditioned'
         AND array_length(provsql.get_children((a)::uuid), 1) = 2
    THEN provsql.agg_token_make(
           (provsql.get_children((a)::uuid))[1], provsql.agg_token_value(a))
    ELSE a
  END;
$$ LANGUAGE sql STABLE PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION variance(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  m1 float8;
  m2 float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    -- Conditioning on prov is handled inside rv_moment: when prov
    -- resolves to gate_one() (the default, or load-time
    -- simplification of any always-true sub-circuit) the
    -- unconditional analytical path runs unchanged; otherwise the
    -- joint-circuit loader unifies shared gate_rv leaves between
    -- input and prov, and the conditional path runs either
    -- truncated-distribution closed form or MC rejection.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), 2, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    m1 := agg_raw_moment(agg_conditioned_target(input::agg_token), 1,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    m2 := agg_raw_moment(agg_conditioned_target(input::agg_token), 2,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    IF m1 IS NULL OR m2 IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN m2 - m1 * m1;
  END IF;

  RAISE EXCEPTION 'variance() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, false,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(agg_conditioned_target(input::agg_token), k,
                          rv_conditioned_prov(input::uuid, prov), method, arguments);
  END IF;

  RAISE EXCEPTION 'moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION support(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL,
  OUT lo float8,
  OUT hi float8)
  AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  values_arr float8[];
  total_probability float8;
BEGIN
  IF input IS NULL THEN
    lo := NULL; hi := NULL; RETURN;
  END IF;

  -- Plain numeric: degenerate point support.  Lets `support(2.5)` /
  -- `support(42)` / etc.  return (2.5, 2.5) without making the user
  -- wrap in `as_random`.
  IF pg_typeof(input) IN (
       'smallint'::regtype, 'integer'::regtype, 'bigint'::regtype,
       'numeric'::regtype, 'real'::regtype, 'double precision'::regtype) THEN
    lo := input::double precision;
    hi := input::double precision;
    RETURN;
  END IF;

  -- random_variable is binary-coercible to uuid (explicit cast
  -- below), so a single rv_support call covers both shapes.
  -- rv_support handles
  -- gate_value (point), gate_rv (distribution), gate_arith
  -- (propagated), and falls back to the conservative all-real
  -- interval for any other gate kind.  Conditioning on prov is not
  -- supported (would require restricting the underlying joint
  -- distribution by the indicator of prov, which has no closed form
  -- for the basic distributions we ship).
  IF pg_typeof(input) IN ('random_variable'::regtype, 'uuid'::regtype) THEN
    -- Conditional support: rv_support folds the AND-conjunct interval
    -- constraints from prov into the unconditional support.  When
    -- prov is gate_one() the unconditional support is returned
    -- unchanged.
    SELECT r.lo, r.hi INTO lo, hi
      FROM provsql.rv_support(
             rv_conditioned_target(input::uuid),
             rv_conditioned_prov(input::uuid, prov)) r;
    RETURN;
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    -- A conditioned aggregate SUM(x)|C: the value-range support is that of
    -- the target aggregate (conditioning can only tighten it; the
    -- conservative range stays valid), so unpack to the target gate.
    DECLARE
      atok agg_token := agg_conditioned_target(input::agg_token);
    BEGIN
    IF get_gate_type(atok) <> 'agg' THEN
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for support computation';
    END IF;
    SELECT pp.proname::varchar FROM pg_proc pp
      WHERE oid=(get_infos(atok)).info1
      INTO aggregation_function;
    child_pairs := get_children(atok);

    IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
      -- count(col) is a SUM of per-row 0/1 indicators (empty group = 0), so its
      -- support is computed like SUM; count(*) arrives as 'sum'.
      -- Empty agg_token: SUM is identically 0.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := 0; hi := 0; RETURN;
      END IF;
      SELECT sum(LEAST(v, 0::float8)), sum(GREATEST(v, 0::float8))
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM unnest(child_pairs) AS c) sub;
    ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
      -- MIN/MAX over the empty input world are NULL, not ±Infinity (matching the
      -- moment surface): the empty world carries no value, so the support is just
      -- the range of the per-row values [min(v), max(v)].  A structurally empty
      -- aggregate has no defined value at all -> NULL support.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := NULL; hi := NULL; RETURN;
      END IF;

      SELECT min(v), max(v)
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM UNNEST(child_pairs) AS c) sub;
    ELSE
      RAISE EXCEPTION USING MESSAGE=
        'Cannot compute support for aggregation function ' || aggregation_function;
    END IF;
    RETURN;
    END;
  END IF;

  RAISE EXCEPTION 'support() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION central_moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  mu float8;
  total float8;
  i integer;
  raw_i float8;
  binom float8;
  -- iterative binomial coefficient C(k, i)
  k_double float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k < 0 THEN
      RAISE EXCEPTION 'central_moment(): k must be non-negative (got %)', k;
    END IF;
    IF k = 0 THEN RETURN 1; END IF;
    IF k = 1 THEN RETURN 0; END IF;

    mu := agg_raw_moment(agg_conditioned_target(input::agg_token), 1,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    IF mu IS NULL THEN RETURN NULL; END IF;
    -- mu may be ±Infinity for empty MIN / MAX with positive empty
    -- probability; central_moment is undefined in that case.
    IF mu = 'Infinity'::float8 OR mu = '-Infinity'::float8 THEN
      RETURN mu;
    END IF;

    total := 0;
    binom := 1;  -- C(k, 0)
    k_double := k;
    FOR i IN 0..k LOOP
      raw_i := agg_raw_moment(agg_conditioned_target(input::agg_token), i,
                              rv_conditioned_prov(input::uuid, prov), method, arguments);
      IF raw_i IS NULL THEN RETURN NULL; END IF;
      total := total + binom * power(-mu, k - i) * raw_i;
      -- C(k, i+1) = C(k, i) * (k - i) / (i + 1)
      IF i < k THEN
        binom := binom * (k_double - i) / (i + 1);
      END IF;
    END LOOP;
    RETURN total;
  END IF;

  RAISE EXCEPTION 'central_moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;
