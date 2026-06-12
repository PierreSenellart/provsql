/**
 * @file
 * @brief ProvSQL PL/pgSQL extension code
 *
 * This file contains the PL/pgSQL code of the ProvSQL extension. This
 * extension requires the standard uuid-ossp extension.
 */

/**
  * @brief <tt>provsql</tt> schema
  *
  * All types and functions introduced by ProvSQL are defined in the
  * provsql schema, requiring prefixing them by <tt>provsql.</tt> or
  * using PostgreSQL's <tt>search_path</tt> variable with a command such
  * as \code{.sql}SET search_path TO public, provsql;\endcode
  */
CREATE SCHEMA provsql;

SET search_path TO provsql;

/**
 * @brief Provenance circuit gate types
 *
 * Each gate in the provenance circuit has a type that determines
 * its semantics during semiring evaluation.
 */
CREATE TYPE provenance_gate AS
  ENUM(
    'input',   -- Input (variable) gate of the circuit
    'plus',    -- Semiring plus
    'times',   -- Semiring times
    'monus',   -- M-Semiring monus
    'project', -- Project gate (for where provenance)
    'zero',    -- Semiring zero
    'one',     -- Semiring one
    'eq',      -- Equijoin gate (for where provenance)
    'agg',     -- Aggregation operator (for aggregate provenance)
    'semimod', -- Semimodule scalar multiplication (for aggregate provenance)
    'cmp',     -- Comparison of aggregate values (HAVING-clause provenance)
    'delta',   -- δ-semiring operator (see Amsterdamer, Deutch, Tannen, PODS 2011)
    'value',   -- Scalar value (for aggregate provenance)
    'mulinput',-- Multivalued input (for Boolean provenance)
    'update',  -- Update operation
    'rv',      -- Continuous random-variable leaf
    'arith',   -- n-ary arithmetic gate over scalar-valued children
    'mixture', -- Probabilistic mixture of two scalar RV roots with a Bernoulli weight
    'assumed', -- Structural assumption marker over a single child: the
                      -- wrapped sub-circuit was computed under the
                      -- assumption named by the gate's extra label --
                      -- 'boolean' (e.g. the safe-query rewrite; the
                      -- historical default when the label is absent) or
                      -- 'absorptive' (cyclic recursion truncated at the
                      -- absorptive value fixpoint).  Transparent for
                      -- evaluation semirings satisfying the assumption,
                      -- fatal error for the rest, rendered as an
                      -- explicit element in PROV-XML export.
    'annotation',     -- Transparent single-child wrapper carrying a
                      -- query-level annotation string in @c extra
                      -- (e.g. the inversion-free tractability
                      -- certificate / per-input order key).  Identity
                      -- for EVERY evaluator; its UUID folds in @c extra
                      -- so distinct annotations over the same child are
                      -- distinct gates.
    'conditioned',    -- Conditioning marker: two children
                      -- [target, evidence].  Evaluated only in the
                      -- measure interpretation: probability_evaluate
                      -- returns P(target ∧ evidence) / P(evidence); the
                      -- RV / agg_token evaluators return the restricted
                      -- distribution.  For the uuid carrier it is a
                      -- TERMINAL gate (never a child of a semiring gate);
                      -- nested conditioning folds into a conjunction of
                      -- evidence.  Refused by every general sr_* semiring
                      -- (normalization is not a semiring operation).
    'mobius'          -- Signed Möbius combination over child islands: one
                      -- integer coefficient per child in @c extra (the
                      -- gate_arith precedent), probability_evaluate returns
                      -- Σ_i coeff_i · P(child_i).  The one new primitive of
                      -- the safe-UCQ Möbius-inversion route, evaluated only
                      -- in the measure interpretation; refused by every
                      -- general sr_* semiring (a signed combination is not a
                      -- semiring operation).
    );

/** @defgroup gate_manipulation Circuit gate manipulation
 *  Low-level functions for creating and querying provenance circuit gates.
 *  @{
 */

/**
 * @brief Create a new gate in the provenance circuit
 *
 * @param token UUID identifying the new gate
 * @param type gate type (see provenance_gate)
 * @param children optional array of child gate UUIDs
 */
CREATE OR REPLACE FUNCTION create_gate(
  token UUID,
  type provenance_gate,
  children uuid[] DEFAULT NULL)
  RETURNS void AS
  'provsql','create_gate' LANGUAGE C PARALLEL SAFE;
/**
 * @brief Return the gate type of a provenance token
 *
 * Returns @c 'input' for any token not yet materialized in the circuit,
 * since input is the default semantics of an unmaterialized provenance token.
 */
CREATE OR REPLACE FUNCTION get_gate_type(
  token UUID)
  RETURNS provenance_gate AS
  'provsql','get_gate_type' LANGUAGE C IMMUTABLE PARALLEL SAFE;
/** @brief Return the children of a provenance gate */
CREATE OR REPLACE FUNCTION get_children(
  token UUID)
  RETURNS uuid[] AS
  'provsql','get_children' LANGUAGE C IMMUTABLE PARALLEL SAFE;
/**
 * @brief Set the probability of an input gate
 *
 * @param token UUID of the input gate
 * @param p probability value in [0,1]
 */
CREATE OR REPLACE FUNCTION set_prob(
  token UUID, p DOUBLE PRECISION)
  RETURNS void AS
  'provsql','set_prob' LANGUAGE C PARALLEL SAFE;
/** @brief Get the probability associated with an input gate */
CREATE OR REPLACE FUNCTION get_prob(
  token UUID)
  RETURNS DOUBLE PRECISION AS
  'provsql','get_prob' LANGUAGE C STABLE PARALLEL SAFE;

/**
 * @brief Set additional integer values on provenance circuit gate
 *
 * This function sets two integer values associated to a circuit gate, used in
 * different ways by different gate types:
 *   - for mulinput, info1 indicates the value of this multivalued variable
 *   - for eq, info1 and info2 indicate the attribute index of the
       equijoin in, respectively, the first and second columns
 *   - for agg, info1 is the oid of the aggregate function and info2 the
       oid of the aggregate result type
 *   - for cmp, info1 is the oid of the comparison operator
 *
 * @param token UUID of the circuit gate
 * @param info1 first integer value
 * @param info2 second integer value
 */
CREATE OR REPLACE FUNCTION set_infos(
  token UUID, info1 INT, info2 INT DEFAULT NULL)
  RETURNS void AS
  'provsql','set_infos' LANGUAGE C PARALLEL SAFE;

/** @brief Get the integer info values associated with a circuit gate */
CREATE OR REPLACE FUNCTION get_infos(
  token UUID, OUT info1 INT, OUT info2 INT)
  RETURNS record AS
  'provsql','get_infos' LANGUAGE C STABLE PARALLEL SAFE;

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

/**
 * @brief Wrap @p token in a fresh transparent @c gate_annotation carrying
 *        @p extra, and return the wrapper's UUID.
 *
 * Unlike every other gate, the annotation wrapper's UUID folds in @p extra
 * (not just the child): @c uuid_generate_v5 over @c concat('annotation',
 * token, extra).  This is deliberate -- two annotations over the same child
 * with different @p extra must be distinct gates (e.g. the same input tuple
 * carrying different per-occurrence order keys, or two queries attaching
 * different certificates to a shared root).  The wrapper is transparent
 * (identity) for EVERY evaluator; @p extra is inert metadata read only by the
 * code that placed it.  No-op (returns NULL) on a NULL input.
 */
CREATE OR REPLACE FUNCTION annotate(token UUID, extra TEXT) RETURNS UUID AS
$$
DECLARE
  annotated uuid;
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  annotated := public.uuid_generate_v5(uuid_ns_provsql(),
                                       concat('annotation', token, extra));
  PERFORM create_gate(annotated, 'annotation', ARRAY[token]);
  PERFORM set_extra(annotated, extra);
  RETURN annotated;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

/**
 * @brief Condition a provenance token (a Boolean event) on another.
 *
 * Builds the terminal @c gate_conditioned that the measure evaluators read
 * as @c "P(target ∧ evidence) / P(evidence)".  This is the backing function
 * of the binary @c | operator (@c "target | evidence", value-level
 * conditioning of the uuid carrier).
 *
 * The gate stores three children @c [target, evidence, joint] with
 * @c joint @c = @c times(target, @c evidence); evaluation is then the plain
 * ratio @c P(joint)/P(evidence), and content-addressing makes a base tuple
 * shared by @p target and @p evidence the same input gate in both circuits,
 * so the conditional is exact and correlation-aware.
 *
 * Conventions:
 *  - Conditioning on a certain or absent event is a no-op: @c evidence NULL
 *    or @c gate_one() returns @p target unchanged (@c "P(X|true)=P(X)").
 *  - A @p target with no provenance defaults to the certain event 1, so
 *    @c "1 | c" is the well-defined certain-row posterior.
 *  - Nested conditioning folds (sequential Bayesian update):
 *    @c "(X | A) | B = X | (A ∧ B)" -- the gate never nests, it stays one
 *    level deep with the evidence accumulated by @c times.
 *
 * The result is TERMINAL: a conditioned token may not become a child of a
 * @c plus / @c times / @c monus / @c agg gate (those constructors refuse
 * it); the only operation it admits is more conditioning.
 */
CREATE OR REPLACE FUNCTION cond(target UUID, evidence UUID) RETURNS UUID AS
$$
DECLARE
  tgt uuid;
  ev  uuid;
  jnt uuid;
  result uuid;
  ch uuid[];
BEGIN
  -- P(X | true) = P(X): conditioning on a certain / absent event is inert.
  IF evidence IS NULL OR evidence = gate_one() THEN
    RETURN target;
  END IF;

  -- A row with no provenance defaults to the certain event 1.
  tgt := coalesce(target, gate_one());

  IF get_gate_type(tgt) = 'conditioned' THEN
    -- Sequential update (X | A) | B = X | (A ∧ B): fold B into both the
    -- evidence and the joint of the inner gate so the result stays a single
    -- gate_conditioned over the ORIGINAL target.
    ch  := get_children(tgt);
    tgt := ch[1];                              -- original target X
    ev  := provenance_times(ch[2], evidence);  -- A ∧ B
    jnt := provenance_times(ch[3], evidence);  -- (X ∧ A) ∧ B
  ELSE
    ev  := evidence;
    jnt := provenance_times(tgt, evidence);    -- X ∧ C
  END IF;

  result := public.uuid_generate_v5(uuid_ns_provsql(),
                                    concat('conditioned', tgt, ev, jnt));
  PERFORM create_gate(result, 'conditioned', ARRAY[tgt, ev, jnt]);
  RETURN result;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

/**
 * @brief Binary @c | : value-level conditioning, @c "target | evidence".
 *
 * Carrier-parametric in its left operand; the uuid form builds the terminal
 * @c gate_conditioned via @c cond.  Does not collide with core PostgreSQL's
 * integer bitwise @c | (different argument types).
 */
CREATE OPERATOR | (LEFTARG=UUID, RIGHTARG=UUID, PROCEDURE=cond);

/**
 * @brief Placeholder for @c "X | (predicate)" on a uuid event.
 *
 * Lets the conditioning event be written as a natural Boolean combination of
 * random_variable / aggregate comparisons (e.g. @c "event | (sensor > 3)")
 * instead of a hand-built gate.  Never executes: the ProvSQL planner hook
 * converts the Boolean operand into a condition gate and emits @c cond.
 */
CREATE OR REPLACE FUNCTION cond_predicate(target UUID, predicate boolean)
  RETURNS UUID AS
$$
BEGIN
  RAISE EXCEPTION 'uuid | (predicate) must be rewritten by the ProvSQL '
    'planner hook: the right operand must be a Boolean combination of '
    'random_variable / aggregate comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR | (LEFTARG=UUID, RIGHTARG=boolean, PROCEDURE=cond_predicate);

/**
 * @brief Deterministic indicator gate for an ordinary (regular) comparison.
 *
 * The predicate-provenance of an ordinary comparison (both sides of regular
 * type, e.g. @c "region = 'north'") is the deterministic indicator
 * @c "χ(cond)": @c gate_one() when the comparison holds on the row,
 * @c gate_zero() otherwise (Definition in the HAVING-provenance semantics).
 * The planner emits this for a regular comparison appearing inside a MIXED
 * conditioning predicate (one that also has a random_variable / aggregate
 * comparison); @c cond is evaluated per row, so the indicator is the row's
 * own truth value, combined by @c ⊗ / @c ⊕ with the probabilistic gates.
 */
CREATE OR REPLACE FUNCTION regular_indicator(cond boolean) RETURNS UUID AS
$$
  SELECT CASE WHEN cond THEN provsql.gate_one() ELSE provsql.gate_zero() END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/**
 * @brief Whole-tuple output conditioning directive: @c "given(evidence)".
 *
 * Written as a term in the select list, @c given(c) conditions the OUTPUT
 * provenance of the current query's rows on @p c:
 *
 * @code
 *   SELECT a, b, given((SELECT provenance() FROM tests
 *                       WHERE patient_id = s.id AND result = 'positive'))
 *   FROM source s;
 *   -- visible columns: a, b   (the given(...) term is stripped)
 *   -- per-row output provenance: provenance() | <that row's evidence>
 * @endcode
 *
 * The query rewriter recognises the marker, STRIPS it from the visible
 * projection, and wraps each output row's provenance expression in
 * @c cond(row_provenance, c) -- deriving a new conditioned relation, never
 * mutating any stored provenance.  @p c is evaluated per output row and may
 * correlate with the row's columns, so each tuple is conditioned on its own
 * evidence.  When the rewriter is inactive the call is a harmless identity
 * (it returns @p evidence as an ordinary column).
 */
CREATE OR REPLACE FUNCTION given(evidence UUID) RETURNS UUID AS
$$
  SELECT evidence;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Prefix unary @c | : alias for @c given, @c "| evidence".
 *
 * Disambiguated from the binary @c | by the absence of a left operand
 * (@c "a, | c" parses @c "| c" as the prefix form).  PostgreSQL keeps
 * prefix operators on every supported version (postfix operators were
 * removed in PG14), so @c "| c" is safe across the CI matrix.
 */
CREATE OPERATOR | (RIGHTARG=UUID, PROCEDURE=given);

/**
 * @brief Placeholder for the prefix @c "| (predicate)" whole-tuple form.
 *
 * Lets the whole-tuple conditioning event be a natural Boolean predicate
 * (e.g. @c "SELECT a, | (sensor > 3) FROM readings") instead of a hand-built
 * gate.  Never executes: the planner converts the Boolean operand into a
 * condition gate and emits @c given, which the rewriter then strips and folds
 * into each output row's provenance.
 */
CREATE OR REPLACE FUNCTION given_predicate(predicate boolean) RETURNS UUID AS
$$
BEGIN
  RAISE EXCEPTION 'prefix | (predicate) must be rewritten by the ProvSQL '
    'planner hook: the operand must be a Boolean combination of '
    'random_variable / aggregate comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR | (RIGHTARG=boolean, PROCEDURE=given_predicate);

/**
 * @brief Event negation: @c "! event" / @c "provenance_not(event)".
 *
 * The complement of a Boolean provenance event: @c "!x" holds in exactly the
 * worlds where @p x does not.  It is sugar for @c "monus(one, x)" -- an
 * ordinary m-semiring expression (Boolean @c NOT, probability @c "1 - P(x)"),
 * NOT a measure-only marker -- so it composes like any @c monus, and a
 * conditioned / terminal token is refused as its child (so @c "!(x | c)"
 * errors, as conditioning cannot be buried under further algebra).
 *
 * The motivating use is conditioning on the NON-occurrence of an arbitrary
 * violation query @p W (a denial constraint), where @p W itself is built with
 * ordinary idioms and needs no hand-rolled gates:
 *
 * @code
 *   -- W = "some pair of overlapping same-room bookings is present"
 *   WITH w AS (SELECT provenance() AS tok
 *              FROM bookings a JOIN bookings b
 *                ON a.id < b.id AND a.room = b.room
 *                   AND a.lo < b.hi AND b.lo < a.hi
 *              GROUP BY ())
 *   SELECT probability_evaluate((SELECT provenance() FROM bookings WHERE id=1)
 *                               | !w.tok)             -- P(booking 1 | no overlap)
 *   FROM w;
 * @endcode
 *
 * Named @c provenance_not, after the @c "provenance_times / _plus / _monus"
 * family; the prefix @c ! operator is the ergonomic form (SQL's reserved
 * @c NOT keyword cannot serve as a function name).
 */
CREATE OR REPLACE FUNCTION provenance_not(event UUID) RETURNS UUID AS
$$
  SELECT provsql.provenance_monus(provsql.gate_one(), event);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
   SET search_path=provsql,pg_temp,public;

/**
 * @brief Prefix unary @c ! : alias for @c provenance_not, @c "! event".
 *
 * Prefix operators are kept on every supported PostgreSQL version (postfix
 * operators were removed in PG14), and core PG defines no prefix @c !  on
 * @c uuid, so @c "! event" is safe across the CI matrix.
 */
CREATE OPERATOR ! (RIGHTARG=UUID, PROCEDURE=provenance_not);

/**
 * @brief Build a per-input order-key string for the inversion-free path.
 *
 * Emitted by the planner per certified atom: @c K-prefixed, length-prefixed
 * @c "K<factor> <octet_length(root)>:<root><octet_length(sec)>:<sec>", parsed
 * back at evaluation by @c safe_cert_key_parse.  @p root / @p sec are the
 * tuple's root- and secondary-class column values (text-cast by the caller);
 * the byte-length prefixes keep the values unambiguous for @em any column type,
 * including text containing spaces, colons or digits.  @p factor is the atom's
 * factor id (or -1 for the shared self-join guard).  @c IMMUTABLE so the planner
 * can fold it and the marker dedups by content-addressing.
 */
CREATE OR REPLACE FUNCTION inversion_free_key(root TEXT, sec TEXT, factor INT)
  RETURNS TEXT AS
$$ SELECT 'K' || factor::text || ' '
       || octet_length(root) || ':' || root
       || octet_length(sec)  || ':' || sec $$
  LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Set extra text information on provenance circuit gate
 *
 * This function sets text-encoded data associated to a circuit gate, used in
 * different ways by different gate types:
 *   - for project, it is a text-encoded ARRAY of two-element ARRAYs that
 *     indicate mappings between input attribute (first element) and output
 *     attribute (second element)
 *   - for value and agg, it is the text-encoded (base for value, computed
 *     for agg) scalar value
 *
 * @param token UUID of the circuit gate
 * @param data text-encoded information
 */
CREATE OR REPLACE FUNCTION set_extra(
  token UUID, data TEXT)
  RETURNS void AS
  'provsql','set_extra' LANGUAGE C PARALLEL SAFE STRICT;
/** @brief Get the text-encoded extra data associated with a circuit gate */
CREATE OR REPLACE FUNCTION get_extra(token UUID)
  RETURNS TEXT AS
  'provsql','get_extra' LANGUAGE C STABLE PARALLEL SAFE RETURNS NULL ON NULL INPUT;

/**
 * @brief Return the total number of materialized gates in the provenance circuit
 *
 * Input gates for provenance-tracked table rows are created lazily on
 * first reference; rows that have never appeared in a query result are
 * not counted.
 */
CREATE OR REPLACE FUNCTION get_nb_gates() RETURNS BIGINT AS
  'provsql', 'get_nb_gates' LANGUAGE C PARALLEL SAFE;

/** @} */

/** @defgroup table_management Provenance table management
 *  Functions for enabling, disabling, and configuring provenance
 *  tracking on user tables.
 *  @{
 */


/**
 * @brief Trigger function for DELETE statement provenance tracking
 *
 * Records the deletion and applies monus to provenance tokens of
 * deleted rows. This is the version for PostgreSQL < 14.
 */
CREATE OR REPLACE FUNCTION delete_statement_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  query_text TEXT;
  delete_token UUID;
  old_token UUID;
  new_token UUID;
  r RECORD;
BEGIN
  delete_token := public.uuid_generate_v4();

  PERFORM create_gate(delete_token, 'input');

  SELECT query
  INTO query_text
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  INSERT INTO delete_provenance (delete_token, query, deleted_by, deleted_at)
  VALUES (delete_token, query_text, current_user, CURRENT_TIMESTAMP);

  EXECUTE format('INSERT INTO %I.%I SELECT * FROM OLD_TABLE;', TG_TABLE_SCHEMA, TG_TABLE_NAME);

  FOR r IN (SELECT * FROM OLD_TABLE) LOOP
    old_token := r.provsql;
    new_token := provenance_monus(old_token, delete_token);

    EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2;', TG_TABLE_SCHEMA, TG_TABLE_NAME)
    USING new_token, old_token;
  END LOOP;

  RETURN NULL;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;


/**
 * @brief Record per-relation provenance metadata used by the
 *        safe-query optimisation.
 *
 * Stores a @c (relid, kind, block_key) record in the persistent
 * mmap-backed table-info store.  @p kind is one of:
 *   - @c 'tid' -- independent input leaves (post-@c add_provenance default)
 *   - @c 'bid' -- block-correlated leaves; rows sharing the same value
 *                 of @p block_key are mutually exclusive.  An empty
 *                 @p block_key means the whole table is one block.
 *   - @c 'opaque' -- arbitrary correlations from a derived source
 *                 (CREATE TABLE AS SELECT, INSERT INTO SELECT,
 *                 UPDATE under provsql.update_provenance); the
 *                 safe-query rewriter must bail on these.
 *
 * @param relid     pg_class OID of the relation.
 * @param kind      One of @c 'tid' / @c 'bid' / @c 'opaque'.
 * @param block_key Block-key column numbers (only meaningful for
 *                  @c 'bid'; ignored otherwise but conventionally
 *                  passed empty).
 */
CREATE OR REPLACE FUNCTION set_table_info(
  relid OID, kind TEXT, block_key INT2[] DEFAULT ARRAY[]::INT2[])
  RETURNS void AS
  'provsql','set_table_info' LANGUAGE C PARALLEL SAFE;

/** @brief Remove per-relation provenance metadata.  No-op when missing. */
CREATE OR REPLACE FUNCTION remove_table_info(relid OID)
  RETURNS void AS
  'provsql','remove_table_info' LANGUAGE C PARALLEL SAFE;

/**
 * @brief Read per-relation provenance metadata.
 *
 * Returns NULL if no record exists.  @c kind is one of @c 'tid' /
 * @c 'bid' / @c 'opaque'; @c block_key is the (possibly empty) array
 * of block-key column numbers, only meaningful when @c kind = @c 'bid'.
 * Used by the planner-time hierarchy detector to gate the safe-query
 * rewrite.
 */
CREATE OR REPLACE FUNCTION get_table_info(
  relid OID, OUT kind TEXT, OUT block_key INT2[])
  RETURNS record AS
  'provsql','get_table_info' LANGUAGE C STABLE PARALLEL SAFE;

/**
 * @brief Record the base-relation ancestor set of a tracked relation.
 *
 * Base tables created with @c add_provenance / @c repair_key carry
 * @c {self}; CTAS-derived tables inherit the union of their sources'
 * ancestor sets.  The safe-query rewriter consults the registry to
 * enforce that joined FROM entries have disjoint base ancestors
 * before firing the read-once factoring.
 *
 * The worker preserves the relation's existing @c kind / @c block_key
 * half on update; it silently no-ops when no kind record exists for
 * @p relid (callers should run @c add_provenance / @c repair_key
 * first).  The ancestor list is capped at 64 entries (clear error if
 * exceeded).
 *
 * @param relid      pg_class OID of the relation.
 * @param ancestors  Sorted, deduplicated base-relation OIDs.
 */
CREATE OR REPLACE FUNCTION set_ancestors(
  relid OID, ancestors OID[] DEFAULT ARRAY[]::OID[])
  RETURNS void AS
  'provsql','set_ancestors' LANGUAGE C PARALLEL SAFE;

/** @brief Clear the ancestor half of a per-relation record (keeps kind/block_key).
 *  No-op when missing. */
CREATE OR REPLACE FUNCTION remove_ancestors(relid OID)
  RETURNS void AS
  'provsql','remove_ancestors' LANGUAGE C PARALLEL SAFE;

/**
 * @brief Read the base-relation ancestor set of a tracked relation.
 *
 * Returns @c NULL when no ancestor record exists for @p relid (or the
 * record is empty -- both cases make the safe-query rewriter take
 * its conservative refuse path, so they collapse here).
 */
CREATE OR REPLACE FUNCTION get_ancestors(relid OID)
  RETURNS OID[] AS
  'provsql','get_ancestors' LANGUAGE C STABLE PARALLEL SAFE;

/**
 * @brief BEFORE INSERT OR UPDATE OF provsql row trigger installed by
 *        @c add_provenance.
 *
 * Two jobs:
 *
 *  1. Fill @c NEW.provsql with a fresh @c uuid_generate_v4 leaf when
 *     the user did not supply one (this replaces the column DEFAULT
 *     that @c add_provenance used to install: a real DEFAULT would
 *     fire before the trigger sees the row, so we could not tell
 *     "user omitted the column" from "user supplied a value").
 *  2. When the user does supply a non-NULL @c provsql on @c INSERT,
 *     or changes it on @c UPDATE, flip the table's per-table
 *     metadata to @c OPAQUE.  The user is free to write whatever
 *     UUIDs they want (cross-table reuse, compound tokens minted
 *     via @c create_gate, ...); the cost is that the safe-query
 *     rewriter then refuses to fire on this table, because TID
 *     independence can no longer be assumed.
 */
CREATE OR REPLACE FUNCTION provenance_guard()
  RETURNS TRIGGER AS $$
BEGIN
  IF TG_OP = 'INSERT' THEN
    IF NEW.provsql IS NULL THEN
      NEW.provsql := public.uuid_generate_v4();
    ELSE
      PERFORM provsql.set_table_info(TG_RELID, 'opaque');
    END IF;
  ELSIF TG_OP = 'UPDATE' THEN
    IF NEW.provsql IS DISTINCT FROM OLD.provsql THEN
      PERFORM provsql.set_table_info(TG_RELID, 'opaque');
    END IF;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER;

/**
 * @brief Enable provenance tracking on an existing table
 *
 * Adds a <tt>provsql</tt> UUID column to the table, an index for
 * fast UUID-keyed lookups, and a BEFORE INSERT/UPDATE row trigger
 * (@c provenance_guard) that mints a fresh @c uuid_generate_v4
 * leaf when the user omits the column on INSERT, or flips the
 * table's metadata to @c OPAQUE when the user supplies their own
 * value.  Input gates for existing rows are created lazily when
 * first referenced by a query.
 *
 * @param _tbl the table to add provenance tracking to
 */
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  -- Idempotence: a second add_provenance on an already-tracked table is
  -- a no-op with a NOTICE, so setup scripts and notebook cells can be
  -- re-run freely.
  IF EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = _tbl AND attname = 'provsql' AND NOT attisdropped
  ) THEN
    RAISE NOTICE 'table % already has provenance tracking', _tbl;
    RETURN;
  END IF;
  -- No DEFAULT: the guard trigger mints the UUID, so the trigger can
  -- distinguish "user omitted" (NULL) from "user supplied a value".
  -- No UNIQUE: we no longer rely on it to keep the table TID -- the
  -- guard does that semantically -- and a UNIQUE would reject the
  -- legitimate cross-table UUID copy that just flips the table to
  -- OPAQUE.  We keep a plain index for fast UUID-keyed lookups.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
  EXECUTE format(
    'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
    _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'tid');
  -- Seed the base-ancestor set to {self}: a base TID table's atoms
  -- come from itself and no other relation.  CTAS-derived tables
  -- inherit unions of source ancestor sets; that is handled by the
  -- CTAS hook (a separate slice), not here.
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql SECURITY DEFINER;

/**
 * @brief Remove provenance tracking from a table
 *
 * Drops the <tt>provsql</tt> column and associated triggers.
 *
 * @param _tbl the table to remove provenance tracking from
 */
CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  PERFORM provsql.remove_table_info(_tbl::oid);
  -- Drop the BEFORE INSERT/UPDATE guard first: it has a column
  -- dependency on provsql (via the OF provsql clause), so the
  -- subsequent DROP COLUMN would otherwise raise.
  BEGIN
    EXECUTE format('DROP TRIGGER provenance_guard on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
  EXECUTE format('ALTER TABLE %s DROP COLUMN provsql', _tbl);
  BEGIN
    EXECUTE format('DROP TRIGGER add_gate on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
  BEGIN
    EXECUTE format('DROP TRIGGER insert_statement on %s', _tbl);
    EXECUTE format('DROP TRIGGER update_statement on %s', _tbl);
    EXECUTE format('DROP TRIGGER delete_statement on %s', _tbl);
  EXCEPTION WHEN undefined_object THEN
  END;
END
$$ LANGUAGE plpgsql;

/**
 * @brief Set up provenance for a table with duplicate key values
 *
 * When a table has duplicate rows for a given key, this function
 * replaces simple input gates with multivalued input (mulinput) gates
 * that model a uniform distribution over duplicates.
 *
 * @param _tbl the table to repair
 * @param key_att the key attribute(s) as a comma-separated string, or
 *        empty string if the whole table is one group
 */
CREATE OR REPLACE FUNCTION repair_key(_tbl regclass, key_att text)
  RETURNS void AS
$$
DECLARE
  r RECORD;
  rows_query TEXT;
  block_key_cols INT2[];
BEGIN
  -- Resolve the (possibly comma-separated) key_att text into the
  -- corresponding pg_attribute.attnum values for the safe-query
  -- metadata.  Names are trimmed; quoting is not supported because
  -- repair_key has never accepted quoted identifiers in key_att.
  IF key_att = '' THEN
    block_key_cols := ARRAY[]::INT2[];
  ELSE
    SELECT array_agg(a.attnum ORDER BY t.ord)::INT2[]
      INTO block_key_cols
      FROM unnest(string_to_array(key_att, ',')) WITH ORDINALITY AS t(name, ord)
      JOIN pg_attribute a
        ON a.attrelid = _tbl
       AND a.attname  = trim(t.name)
       AND a.attnum   > 0
       AND NOT a.attisdropped;
    IF block_key_cols IS NULL OR array_length(block_key_cols, 1) IS NULL THEN
      RAISE EXCEPTION 'repair_key: could not resolve key columns from "%"', key_att;
    END IF;
    IF array_length(block_key_cols, 1) > 16 THEN
      RAISE EXCEPTION 'repair_key: block key wider than 16 columns is not supported';
    END IF;
  END IF;

  -- Same column shape as add_provenance: no UNIQUE, no DEFAULT past
  -- the initial backfill (the guard trigger added after the rename
  -- takes over both jobs once the column has been renamed to its
  -- final name).  The DEFAULT is kept here only so the second pass
  -- below can read provsql_temp from the user-visible rows
  -- without a separate UPDATE.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql_temp UUID DEFAULT public.uuid_generate_v4()', _tbl);

  -- Build a per-group mapping (key columns + a fresh key_token + the
  -- group size) once, then use it for both the create_gate(key_token,
  -- 'input') first pass and the per-row mulinput second pass.  Going
  -- through a temp table avoids re-running uuid_generate_v4() (which
  -- would produce different UUIDs the second time).  USING (%1$s) on
  -- the second pass handles the multi-column case uniformly.
  -- ON COMMIT DROP plus the explicit DROP TABLE at the end of this
  -- function leave the temp table cleaned up across transactions and
  -- across repeated calls in the same transaction.
  IF key_att = '' THEN
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %s', _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %s t CROSS JOIN provsql_repair_key_tmp k', _tbl);
  ELSE
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT %1$s,
                public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %2$s
       GROUP BY %1$s', key_att, _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (PARTITION BY k.provsql_key_token
                                 ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %2$s t
         JOIN provsql_repair_key_tmp k USING (%1$s)', key_att, _tbl);
  END IF;

  -- Pass 1: one input gate per group key.
  FOR r IN SELECT provsql_key_token FROM provsql_repair_key_tmp LOOP
    PERFORM provsql.create_gate(r.provsql_key_token, 'input');
  END LOOP;

  -- Pass 2: per row, attach a mulinput gate to its group's key token.
  FOR r IN EXECUTE rows_query LOOP
    PERFORM provsql.create_gate(r.provsql_temp, 'mulinput', ARRAY[r.key_token]);
    PERFORM provsql.set_prob(r.provsql_temp, 1./r.group_size);
    PERFORM provsql.set_infos(r.provsql_temp, r.within_group::int);
  END LOOP;

  DROP TABLE provsql_repair_key_tmp;

  EXECUTE format('ALTER TABLE %s ALTER COLUMN provsql_temp DROP DEFAULT', _tbl);
  EXECUTE format('ALTER TABLE %s RENAME COLUMN provsql_temp TO provsql', _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'bid', block_key_cols);
  -- Base BID tables also have themselves as their sole ancestor.  Same
  -- rationale as the @c add_provenance branch above.
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql;

/**
 * @brief Event trigger that purges per-table provenance metadata when
 *        a tracked relation is dropped outside of remove_provenance().
 *
 * Plain DROP TABLE bypasses remove_provenance() and would otherwise
 * leave a stale entry in the table-info store keyed by a now-recycled
 * OID, with confusing consequences for the safe-query rewriter the
 * next time the OID is reused.  This trigger forwards every dropped
 * relation OID to provsql.remove_table_info(), which is a no-op for
 * relations that were not tracked.
 */
CREATE OR REPLACE FUNCTION cleanup_table_info()
  RETURNS event_trigger AS
$$
DECLARE
  r RECORD;
BEGIN
  FOR r IN
    SELECT objid FROM pg_event_trigger_dropped_objects()
     WHERE object_type IN ('table', 'foreign table', 'materialized view')
  LOOP
    PERFORM provsql.remove_table_info(r.objid);
  END LOOP;
END
$$ LANGUAGE plpgsql;

DROP EVENT TRIGGER IF EXISTS provsql_cleanup_table_info;
-- @c EXECUTE @c PROCEDURE (rather than the PG 11+ @c EXECUTE
-- @c FUNCTION alias) so the extension installs on PG 10 too.
CREATE EVENT TRIGGER provsql_cleanup_table_info ON sql_drop
  EXECUTE PROCEDURE provsql.cleanup_table_info();

/**
 * @brief Create a provenance mapping table from an attribute
 *
 * Creates a new table mapping provenance tokens to values of a given
 * attribute, for use with semiring evaluation functions.
 * Idempotent: if the mapping table already exists, raises a NOTICE and
 * changes nothing (drop it first to rebuild).
 *
 * @param newtbl name of the mapping table to create
 * @param oldtbl source table with provenance tracking
 * @param att attribute whose values populate the mapping
 * @param preserve_case if true, quote the table name to preserve case
 */
CREATE OR REPLACE FUNCTION create_provenance_mapping(
  newtbl text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT 'f'
) RETURNS void AS
$$
DECLARE
BEGIN
  -- Idempotence: when the mapping table already exists, leave it alone
  -- with a NOTICE (re-runnable setup scripts / notebook cells). Drop it
  -- first to rebuild a stale mapping.
  IF (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
           ELSE to_regclass(newtbl) END) IS NOT NULL THEN
    RAISE NOTICE 'mapping table % already exists', newtbl;
    RETURN;
  END IF;
  -- ON COMMIT DROP only fires at COMMIT: several mapping creations in
  -- one transaction (a notebook cell, a setup script run via psql -1)
  -- would otherwise collide on the leftover temp table. The to_regclass
  -- probe (rather than DROP IF EXISTS) keeps the first call NOTICE-free.
  IF to_regclass('pg_temp.tmp_provsql') IS NOT NULL THEN
    DROP TABLE tmp_provsql;
  END IF;
  EXECUTE format('CREATE TEMP TABLE tmp_provsql ON COMMIT DROP AS TABLE %s', oldtbl);
  ALTER TABLE tmp_provsql RENAME provsql TO provenance;
  IF preserve_case THEN
    EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
  ELSE
    EXECUTE format('CREATE TABLE %s AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %s(provenance)', newtbl);
  END IF;
END
$$ LANGUAGE plpgsql;

/**
 * @brief Create a view mapping provenance tokens to attribute values
 *
 * Like create_provenance_mapping but creates a view instead of a table,
 * so it always reflects the current state of the source table.
 *
 * @param newview name of the view to create
 * @param oldtbl source table with provenance tracking
 * @param att attribute whose values populate the mapping
 * @param preserve_case if true, quote the view name to preserve case
 */
CREATE OR REPLACE FUNCTION create_provenance_mapping_view(
  newview text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT false
)
RETURNS void
LANGUAGE plpgsql
AS
$$
BEGIN
  IF preserve_case THEN
    EXECUTE format(
      'CREATE OR REPLACE VIEW %I AS SELECT %s AS value, provsql AS provenance FROM %s',
      newview,
      att,
      oldtbl
    );
  ELSE
    EXECUTE format(
      'CREATE OR REPLACE VIEW %s AS SELECT %s AS value, provsql AS provenance FROM %s',
      newview,
      att,
      oldtbl
    );
  END IF;
END;
$$;

/** @} */

/** @defgroup internal_constants Internal constants
 *  UUID namespace and identity element functions used for
 *  deterministic gate generation.
 *  @{
 */

/** @brief Return the ProvSQL UUID namespace (used for deterministic gate UUIDs) */
CREATE OR REPLACE FUNCTION uuid_ns_provsql() RETURNS uuid AS
$$
 -- uuid_generate_v5(uuid_ns_url(),'http://pierre.senellart.com/software/provsql/')
 SELECT '920d4f02-8718-5319-9532-d4ab83a64489'::uuid
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

/** @brief Return the UUID of the semiring zero gate */
CREATE OR REPLACE FUNCTION gate_zero() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(),'zero');
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

/** @brief Return the UUID of the semiring one gate */
CREATE OR REPLACE FUNCTION gate_one() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(),'one');
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

/** @brief Return the epsilon threshold used for probability comparisons */
CREATE OR REPLACE FUNCTION epsilon() RETURNS DOUBLE PRECISION AS
$$
  SELECT CAST(0.001 AS DOUBLE PRECISION)
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

/** @} */

/** @defgroup semiring_operations Semiring operations
 *  Functions that build provenance circuit gates for semiring operations.
 *  These are called internally by the query rewriter.
 *  @{
 */

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
 * @brief Create a monus (difference) gate from two provenance tokens
 *
 * Implements m-semiring monus. Returns token1 if token2 is NULL
 * (used for LEFT OUTER JOIN semantics in the EXCEPT rewriting).
 */
CREATE OR REPLACE FUNCTION provenance_monus(token1 UUID, token2 UUID)
  RETURNS UUID AS
$$
DECLARE
  monus_token uuid;
BEGIN
  IF token1 IS NULL THEN
    RAISE EXCEPTION USING MESSAGE='provenance_monus is called with first argument NULL';
  END IF;

  IF token2 IS NULL THEN
    -- Special semantics, because of a LEFT OUTER JOIN used by the
    -- difference operator: token2 NULL means there is no second argument
    RETURN token1;
  END IF;

  IF token1 = token2 THEN
    -- X-X=0
    monus_token:=gate_zero();
  ELSIF token1 = gate_zero() THEN
    -- 0-X=0
    monus_token:=gate_zero();
  ELSIF token2 = gate_zero() THEN
    -- X-0=X
    monus_token:=token1;
  ELSE
    monus_token:=uuid_generate_v5(uuid_ns_provsql(),concat('monus',token1,token2));
    PERFORM create_gate(monus_token, 'monus', ARRAY[token1::uuid, token2::uuid]);
  END IF;

  RETURN monus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

/**
 * @brief Create a project gate for where-provenance tracking
 *
 * Records the mapping between input and output attribute positions.
 *
 * @param token child provenance token
 * @param positions array encoding attribute position mappings
 */
CREATE OR REPLACE FUNCTION provenance_project(token UUID, VARIADIC positions int[])
  RETURNS UUID AS
$$
DECLARE
  project_token uuid;
  rec record;
BEGIN
  project_token:=uuid_generate_v5(uuid_ns_provsql(),concat('project', token, positions));
  PERFORM create_gate(project_token, 'project', ARRAY[token]);
  PERFORM set_extra(project_token, ARRAY_AGG(pair)::text)
  FROM (
    SELECT ARRAY[(CASE WHEN info=0 THEN NULL ELSE info END), idx] AS pair
    FROM unnest(positions) WITH ORDINALITY AS a(info, idx)
    ORDER BY idx
  ) t;

  RETURN project_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

/**
 * @brief Create an equijoin gate for where-provenance tracking
 *
 * @param token child provenance token
 * @param pos1 attribute index in the first relation
 * @param pos2 attribute index in the second relation
 */
CREATE OR REPLACE FUNCTION provenance_eq(token UUID, pos1 int, pos2 int)
  RETURNS UUID AS
$$
DECLARE
  eq_token uuid;
  rec record;
BEGIN
  eq_token:=uuid_generate_v5(uuid_ns_provsql(),concat('eq',token,pos1,',',pos2));

  PERFORM create_gate(eq_token, 'eq', ARRAY[token::uuid]);
  PERFORM set_infos(eq_token, pos1, pos2);
  RETURN eq_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

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

/**
 * @brief Create a comparison gate for HAVING clause provenance
 *
 * @param left_token provenance token for the left operand
 * @param comparison_op OID of the comparison operator
 * @param right_token provenance token for the right operand
 */
CREATE OR REPLACE FUNCTION provenance_cmp(
  left_token  UUID,
  comparison_op OID,
  right_token UUID
)
RETURNS UUID AS
$$
DECLARE
  cmp_token UUID;
BEGIN
  -- deterministic v5 namespace id
  cmp_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('cmp', left_token::text, comparison_op::text, right_token::text)
  );
  -- wire it up in the circuit
  PERFORM create_gate(cmp_token, 'cmp', ARRAY[left_token, right_token]);
  PERFORM set_infos(cmp_token, comparison_op::integer);
  RETURN cmp_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE
  STRICT;

/**
 * @brief Create an arithmetic gate over scalar-valued provenance children
 *
 * Builds a deterministic @c gate_arith from an operator tag and an
 * ordered list of children.  The tag is one of the @c provsql_arith_op
 * enum values declared in @c src/provsql_utils.h
 * (@c PLUS=0, @c TIMES=1, @c MINUS=2, @c DIV=3, @c NEG=4) and is
 * stored in the gate's @c info1 field.  Children must be UUIDs of
 * scalar-producing gates (@c gate_rv, @c gate_value, or another
 * @c gate_arith).  The token UUID is derived deterministically from
 * @p op and @p children so identical sub-expressions share their gate.
 *
 * @param op        Operator tag (@c provsql_arith_op).
 * @param children  Ordered list of child gate UUIDs.
 * @return  UUID of the (possibly pre-existing) @c gate_arith.
 */
CREATE OR REPLACE FUNCTION provenance_arith(
  op       INTEGER,
  children UUID[]
)
RETURNS UUID AS
$$
DECLARE
  arith_token UUID;
BEGIN
  arith_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('arith', op::text, children::text)
  );
  PERFORM create_gate(arith_token, 'arith', children);
  PERFORM set_infos(arith_token, op);
  RETURN arith_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE
  STRICT;

/** @} */

/** @defgroup semiring_evaluation Semiring evaluation
 *  Functions for evaluating provenance circuits over semirings,
 *  both user-defined (via function references) and compiled (built-in).
 *  @{
 */

/**
 * @brief Evaluate provenance using a compiled (built-in) semiring
 *
 * This C function handles semiring evaluation entirely in C++ for
 * better performance. The semiring is specified by name.
 *
 * @param token provenance token to evaluate
 * @param token2value mapping table from tokens to semiring values
 * @param semiring name of the compiled semiring (e.g., "formula", "counting")
 * @param element_one identity element of the semiring
 */
CREATE OR REPLACE FUNCTION provenance_evaluate_compiled(
  token UUID,
  token2value regclass,
  semiring TEXT,
  element_one anyelement)
RETURNS anyelement AS
  'provsql', 'provenance_evaluate_compiled' LANGUAGE C PARALLEL SAFE STABLE;


/**
 * @brief Evaluate provenance over a user-defined semiring (PL/pgSQL version)
 *
 * Recursively walks the provenance circuit and evaluates each gate
 * using the provided semiring operations. This is the generic version
 * that accepts semiring operations as function references.
 *
 * @param token provenance token to evaluate
 * @param token2value mapping table from tokens to semiring values
 * @param element_one identity element of the semiring
 * @param value_type OID of the semiring value type
 * @param plus_function semiring addition (aggregate)
 * @param times_function semiring multiplication (aggregate)
 * @param monus_function semiring monus (binary), or NULL
 * @param delta_function δ-semiring operator, or NULL
 */
CREATE OR REPLACE FUNCTION provenance_evaluate(
  token UUID,
  token2value regclass,
  element_one anyelement,
  value_type regtype,
  plus_function regproc,
  times_function regproc,
  monus_function regproc,
  delta_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  gate_type provenance_gate;
  result ALIAS FOR $0;
  children UUID[];
--  cmp_value anyelement;
--  temp_result anyelement;
  value_text TEXT;
BEGIN
  SELECT get_gate_type(token) INTO gate_type;

  IF gate_type IS NULL THEN
    RETURN NULL;

  ELSIF gate_type = 'input' THEN
    EXECUTE format('SELECT value FROM %s WHERE provenance=%L', token2value, token)
      INTO result;
    IF result IS NULL THEN
      result := element_one;
    END IF;
  ELSIF gate_type = 'mulinput' THEN
    SELECT concat('{',(get_children(token))[1]::text,'=',(get_infos(token)).info1,'}')
      INTO result;
  ELSIF gate_type='update' THEN
    EXECUTE format('SELECT value FROM %s WHERE provenance=%L',token2value,token) INTO result;
    IF result IS NULL THEN
      result:=element_one;
    END IF;
  ELSIF gate_type = 'plus' THEN
    EXECUTE format('SELECT %s(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      plus_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;

  ELSIF gate_type = 'times' THEN
    EXECUTE format('SELECT %s(provsql.provenance_evaluate(t,%L,%L::%s,%L,%L,%L,%L,%L)) FROM unnest(get_children(%L)) AS t',
      times_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;

  ELSIF gate_type = 'monus' THEN
    IF monus_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with negation evaluated over a semiring without monus function';
    ELSE
      EXECUTE format('SELECT %s(a1,a2) FROM (SELECT provsql.provenance_evaluate(c[1],%L,%L::%s,%L,%L,%L,%L,%L) AS a1, ' ||
                     'provsql.provenance_evaluate(c[2],%L,%L::%s,%L,%L,%L,%L,%L) AS a2 FROM get_children(%L) c) tmp',
        monus_function, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function,
        token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function, token)
      INTO result;
    END IF;

  ELSIF gate_type = 'eq' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

/*  elsif gate_type = 'cmp' then

    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO temp_result;

    EXECUTE format('SELECT get_extra((get_children(%L))[2])', token)
      INTO cmp_value;

    IF temp_result::text = cmp_value::text THEN
      SELECT concat('{',temp_result::text,'=',cmp_value::text,'}')
      INTO result;
    ELSE
      RETURN gate_zero()
      */



  ELSIF gate_type = 'delta' THEN
    IF delta_function IS NULL THEN
      RAISE EXCEPTION USING MESSAGE='Provenance with aggregation evaluated over a semiring without delta function';
    ELSE
      EXECUTE format('SELECT %I(a) FROM (SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L) AS a) tmp',
        delta_function, token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;
    END IF;

  ELSIF gate_type = 'zero' THEN
    EXECUTE format('SELECT %I(a) FROM (SELECT %L::%I AS a WHERE FALSE) temp', plus_function, element_one, value_type)
      INTO result;

  ELSIF gate_type = 'one' THEN
    EXECUTE format('SELECT %L::%I', element_one, value_type)
      INTO result;

  ELSIF gate_type = 'project' THEN
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

  ELSIF gate_type = 'annotation' THEN
    -- Transparent single-child wrapper (carries the inversion-free certificate
    -- / per-input order keys in extra, inert for every semiring): evaluate
    -- through to the child, like 'project'.
    EXECUTE format('SELECT provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L)',
      token, token2value, element_one, value_type, value_type, plus_function, times_function, monus_function, delta_function)
      INTO result;

  ELSE
    RAISE EXCEPTION USING MESSAGE='provenance_evaluate cannot be called on formulas using ' || gate_type || ' gates; use compiled semirings instead';
  END IF;

  RETURN result;
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;


/**
 * @brief Evaluate aggregate provenance over a user-defined semiring (PL/pgSQL version)
 *
 * Handles agg and semimod gates produced by GROUP BY queries.
 *
 * @param token provenance token to evaluate
 * @param token2value mapping table from tokens to semiring values
 * @param agg_function_final finalization function for the aggregate
 * @param agg_function aggregate combination function
 * @param semimod_function semimodule scalar multiplication function
 * @param element_one identity element of the semiring
 * @param value_type OID of the semiring value type
 * @param plus_function semiring addition
 * @param times_function semiring multiplication
 * @param monus_function semiring monus, or NULL
 * @param delta_function δ-semiring operator, or NULL
 */
CREATE OR REPLACE FUNCTION aggregation_evaluate(
  token UUID,
  token2value regclass,
  agg_function_final regproc,
  agg_function regproc,
  semimod_function regproc,
  element_one anyelement,
  value_type regtype,
  plus_function regproc,
  times_function regproc,
  monus_function regproc,
  delta_function regproc)
  RETURNS anyelement AS
$$
DECLARE
  gt provenance_gate;
  result ALIAS FOR $0;
BEGIN
  SELECT get_gate_type(token) INTO gt;

  IF gt IS NULL THEN
    RETURN NULL;
  ELSIF gt='agg' THEN
    EXECUTE format('SELECT %I(%I(provsql.aggregation_evaluate(t,%L,%L,%L,%L,%L::%s,%L,%L,%L,%L,%L)),pp.proname::varchar) FROM
                    unnest(get_children(%L)) AS t, pg_proc pp
                    WHERE pp.oid=(get_infos(%L)).info1
                    GROUP BY pp.proname',
      agg_function_final, agg_function,token2value,agg_function_final,agg_function,semimod_function,element_one,value_type,value_type,plus_function,times_function,
      monus_function,delta_function,token,token)
    INTO result;
  ELSE
    -- gt='semimod'
    EXECUTE format('SELECT %I(get_extra((get_children(%L))[2]),provsql.provenance_evaluate((get_children(%L))[1],%L,%L::%s,%L,%L,%L,%L,%L))',
      semimod_function,token,token,token2value,element_one,value_type,value_type,plus_function,times_function,monus_function,delta_function)
    INTO result;
  END IF;
  RETURN result;
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

/**
 * @brief Evaluate provenance over a user-defined semiring (C version)
 *
 * Optimized C implementation of provenance_evaluate. Infers the
 * value type from element_one. Monus and delta functions are optional.
 *
 * @param token provenance token to evaluate
 * @param token2value mapping table from tokens to semiring values
 * @param element_one identity element of the semiring
 * @param plus_function semiring addition (aggregate)
 * @param times_function semiring multiplication (aggregate)
 * @param monus_function semiring monus, or NULL if not needed
 * @param delta_function δ-semiring operator, or NULL if not needed
 */
CREATE OR REPLACE FUNCTION provenance_evaluate(
  token UUID,
  token2value regclass,
  element_one anyelement,
  plus_function regproc,
  times_function regproc,
  monus_function regproc = NULL,
  delta_function regproc = NULL)
  RETURNS anyelement AS
  'provsql','provenance_evaluate' LANGUAGE C STABLE;

/** @brief Evaluate aggregate provenance over a user-defined semiring (C version) */
CREATE OR REPLACE FUNCTION aggregation_evaluate(
  token UUID,
  token2value regclass,
  agg_function_final regproc,
  agg_function regproc,
  semimod_function regproc,
  element_one anyelement,
  plus_function regproc,
  times_function regproc,
  monus_function regproc = NULL,
  delta_function regproc = NULL)
  RETURNS anyelement AS
  'provsql','aggregation_evaluate' LANGUAGE C STABLE;

/** @} */

/** @defgroup circuit_introspection Circuit introspection
 *  Functions for examining the structure of provenance circuits,
 *  used by visualization and where-provenance features.
 *  @{
 */

/** @brief Row type for sub_circuit_with_desc results */
CREATE TYPE gate_with_desc AS (f UUID, t UUID, gate_type provenance_gate, desc_str CHARACTER VARYING, infos INTEGER[], extra TEXT);

/**
 * @brief Return the sub-circuit reachable from a token, with descriptions
 *
 * Recursively traverses the provenance circuit from the given token and
 * returns all edges together with input gate descriptions from the
 * mapping table.
 *
 * @param token root provenance token
 * @param token2desc mapping table providing descriptions for input gates
 */
CREATE OR REPLACE FUNCTION sub_circuit_with_desc(
  token UUID,
  token2desc regclass) RETURNS SETOF gate_with_desc AS
$$
BEGIN
  RETURN QUERY EXECUTE
    'WITH RECURSIVE transitive_closure(f,t,gate_type) AS (
      SELECT $1,t,provsql.get_gate_type($1) FROM unnest(provsql.get_children($1)) AS t
        UNION ALL
      SELECT p1.t,u,provsql.get_gate_type(p1.t) FROM transitive_closure p1, unnest(provsql.get_children(p1.t)) AS u)
    SELECT *, ARRAY[(get_infos(f)).info1, (get_infos(f)).info2], get_extra(f) FROM (
      SELECT f::uuid,t::uuid,gate_type,NULL FROM transitive_closure
        UNION ALL
      SELECT p2.provenance::uuid as f, NULL::uuid, ''input'', CAST (p2.value AS varchar) FROM transitive_closure p1 JOIN ' || token2desc || ' AS p2
        ON p2.provenance=t
        UNION ALL
      SELECT provenance::uuid as f, NULL::uuid, ''input'', CAST (value AS varchar) FROM ' || token2desc || ' WHERE provenance=$1
    ) t'
  USING token LOOP;
  RETURN;
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

/**
 * @brief Identify which table and how many columns a provenance token belongs to
 *
 * Searches all provenance-tracked tables for a row matching the given
 * token and returns the table name and column count.
 *
 * @param token provenance token to look up
 * @param table_name (OUT) the table containing this token
 * @param nb_columns (OUT) number of non-provenance columns in that table
 */
CREATE OR REPLACE FUNCTION identify_token(
  token UUID, OUT table_name regclass, OUT nb_columns integer) AS
$$
DECLARE
  t RECORD;
  result RECORD;
BEGIN
  table_name:=NULL;
  nb_columns:=-1;
  FOR t IN
    SELECT relname,
      (SELECT count(*) FROM pg_attribute a2 WHERE a2.attrelid=a1.attrelid AND attnum>0 AND atttypid<>0)-1 c
    FROM pg_attribute a1 JOIN pg_type ON atttypid=pg_type.oid
                        JOIN pg_class ON attrelid=pg_class.oid
                        JOIN pg_namespace ON relnamespace=pg_namespace.oid
    WHERE typname='uuid' AND relkind='r'
                                     AND nspname<>'provsql'
                                     AND attname='provsql'
  LOOP
    EXECUTE format('SELECT * FROM %I WHERE provsql=%L',t.relname,token) INTO result;
    -- Test result.provsql rather than the whole record: "RECORD IS NOT NULL"
    -- is true only when every field is non-null, so a matched row that has any
    -- NULL data column would be wrongly skipped. The provsql column is the
    -- (non-null) token we matched on, so it is set iff a row was found.
    IF result.provsql IS NOT NULL THEN
      table_name:=t.relname;
      nb_columns:=t.c;
      EXIT;
    END IF;
  END LOOP;
END
$$ LANGUAGE plpgsql STRICT;

/**
 * @brief Return the sub-circuit for where-provenance computation
 *
 * Similar to sub_circuit_with_desc but resolves input gates to their
 * source table and column count for where-provenance evaluation.
 */
CREATE OR REPLACE FUNCTION sub_circuit_for_where(token UUID)
  RETURNS TABLE(f UUID, t UUID, gate_type provenance_gate, table_name REGCLASS, nb_columns INTEGER, infos INTEGER[], extra TEXT) AS
$$
    WITH RECURSIVE transitive_closure(f,t,idx,gate_type) AS (
      SELECT $1,t,id,provsql.get_gate_type($1) FROM unnest(provsql.get_children($1)) WITH ORDINALITY AS a(t,id)
        UNION ALL
      SELECT p1.t,u,id,provsql.get_gate_type(p1.t) FROM transitive_closure p1, unnest(provsql.get_children(p1.t)) WITH ORDINALITY AS a(u, id)
    ) SELECT f, t, gate_type, table_name, nb_columns, ARRAY[(get_infos(f)).info1, (get_infos(f)).info2], get_extra(f) FROM (
      -- One row per distinct (parent, child, child-position) edge.  The
      -- recursive closure (UNION ALL) re-emits a gate's outgoing edges once per
      -- path that reaches it, so a *shared* non-input gate would otherwise be
      -- reported with duplicate edges; DISTINCT on the (f,t,idx) triple
      -- collapses those while keeping genuine repeated children (same f,t,
      -- different idx, e.g. a self-product).  Without this, a shared
      -- single-child gate (notably an inversion-free order-marker annotation)
      -- gets its child wired k times in the where-circuit -> the locator sets
      -- are duplicated k-fold.
      SELECT DISTINCT f, t::uuid, idx, gate_type, NULL::regclass AS table_name, NULL::integer AS nb_columns FROM transitive_closure
      UNION ALL
        SELECT DISTINCT t, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM transitive_closure JOIN (SELECT t AS prov, provsql.identify_token(t) as id FROM transitive_closure WHERE t NOT IN (SELECT f FROM transitive_closure)) temp ON t=prov
      UNION ALL
        SELECT DISTINCT $1, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM (SELECT provsql.identify_token($1) AS id WHERE $1 NOT IN (SELECT f FROM transitive_closure)) temp
      ) t
    -- order each parent's edges by child position so the where-circuit's TIMES
    -- concatenation reproduces the column order (input rows have idx NULL).
    ORDER BY f, idx
$$
LANGUAGE sql;

/**
 * @brief BFS expansion of a provenance circuit, capped at @p max_depth
 *
 * Returns one row per (parent, child) edge in the BFS-bounded subgraph
 * rooted at @p root, plus one row for the root with <tt>parent</tt> and
 * <tt>child_pos</tt> NULL.  Provenance circuits are DAGs, so a child gate
 * may have several parents within the bound; each such edge is reported
 * as a separate row, so callers must deduplicate on <tt>node</tt> if they
 * need a one-row-per-node view.
 *
 * <tt>depth</tt> is the node's BFS depth (its shortest distance from
 * @p root), so for an edge (parent, child) it is always the case that
 * <tt>parent.depth + 1 &gt;= child.depth</tt>; equality holds only on
 * shortest-path edges.  A node at <tt>depth = max_depth</tt> is not
 * expanded; callers can detect a partial expansion by comparing
 * <tt>provsql.get_children</tt> length against the number of outgoing
 * edges reported.
 *
 * <tt>info1</tt> and <tt>info2</tt> are the integer values stored on
 * the gate by <tt>provsql.set_infos</tt>, formatted as text; their
 * meaning is gate-type-specific (see <tt>provsql.set_infos</tt>).
 *
 * @param root root provenance token
 * @param max_depth maximum BFS depth (default 8)
 */
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

/**
 * @brief BFS subgraph of the IN-MEMORY simplified circuit rooted at @p root.
 *
 * Same row shape as @ref circuit_subgraph plus an inline @c extra
 * column, but built from the @c GenericCircuit returned by
 * @c getGenericCircuit -- i.e. AFTER @c provsql.simplify_on_load
 * passes (RangeCheck, ...) have rewritten any decidable @c gate_cmp
 * into Bernoulli @c gate_input / @c gate_zero / @c gate_one leaves.
 * Lets a renderer show the user what the evaluator actually sees,
 * without mutating the persisted DAG.
 *
 * Returns @c jsonb (an array of objects) rather than @c SETOF record
 * to keep the C++ implementation free of SRF / @c FuncCallContext
 * boilerplate; callers either consume the array directly or expand
 * it via @c jsonb_array_elements.
 *
 * @param root      Root provenance token.
 * @param max_depth Maximum BFS depth (default 8).
 */
CREATE OR REPLACE FUNCTION simplified_circuit_subgraph(
  root UUID, max_depth INT DEFAULT 8) RETURNS jsonb
  AS 'provsql','simplified_circuit_subgraph'
  LANGUAGE C STABLE PARALLEL SAFE;

/**
 * @brief Empirical histogram of a scalar sub-circuit
 *
 * Returns a jsonb array of @c {bin_lo, bin_hi, count} objects covering
 * the observed @c [min, max] range of @p bins equal-width samples from
 * the sub-circuit rooted at @p token.  Sample count is taken from
 * @c provsql.rv_mc_samples; pinning @c provsql.monte_carlo_seed makes
 * the result reproducible.
 *
 * Accepted root gate types are the scalar ones: @c gate_value (Dirac
 * at the constant, single bin), @c gate_rv (sampled from the leaf's
 * distribution), and @c gate_arith (sampled by recursing through the
 * arithmetic DAG, with shared @c gate_rv leaves correctly correlated
 * within an iteration).  Any other gate type raises.
 *
 * @param token Root provenance token of a scalar sub-circuit.
 * @param bins  Number of equal-width histogram bins (default 30).
 * @param prov  Conditioning event (defaults to @c gate_one() = no
 *              conditioning).  When non-trivial, the histogram is
 *              over the conditional distribution recovered by
 *              rejection sampling on the joint circuit with @p token.
 */
CREATE OR REPLACE FUNCTION rv_histogram(
  token UUID, bins INT DEFAULT 30, prov UUID DEFAULT gate_one())
  RETURNS jsonb
  AS 'provsql','rv_histogram'
  LANGUAGE C VOLATILE PARALLEL SAFE;

/**
 * @brief Sample the closed-form PDF and CDF of a (possibly truncated)
 *        scalar distribution.
 *
 * Returns @c {"pdf": [{x, p}, ...], "cdf": [{x, p}, ...]} with @p samples
 * evenly-spaced points spanning the distribution's natural display
 * range (intersected with the conditioning event's interval when
 * @c prov is non-trivial).  Used by ProvSQL Studio's Distribution
 * profile panel to overlay the analytical curve on the empirical
 * histogram from :sqlfunc:`rv_histogram` -- the simplifier's
 * analytical wins (e.g. @c c·Exp(λ) folding to @c Exp(λ/c)) become
 * visible as a smooth curve riding over the MC-sampled bars.
 *
 * Returns @c NULL when the root sub-circuit is not a closed-form
 * shape (V1: only bare @c gate_rv of Normal / Uniform / Exponential
 * / integer-Erlang).  The frontend reads @c NULL as "skip overlay"
 * without erroring, so the caller can dispatch this in parallel with
 * @c rv_histogram regardless of the underlying shape.
 *
 * @param token   Scalar gate token (random_variable's UUID).
 * @param samples Number of (x, p) points; must be >= 2.
 * @param prov    Conditioning event (defaults to @c gate_one() = no
 *                conditioning).  When non-trivial, the curves are
 *                over the truncated distribution.
 */
CREATE OR REPLACE FUNCTION rv_analytical_curves(
  token UUID, samples INT DEFAULT 100, prov UUID DEFAULT gate_one())
  RETURNS jsonb
  AS 'provsql','rv_analytical_curves'
  LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Draw conditional Monte Carlo samples from a scalar gate.
 *
 * Returns up to @c n samples of the scalar value at @c token; when
 * @c prov is not the trivial @c gate_one() event, draws are accepted
 * only on iterations where @c prov evaluates true (rejection
 * sampling).  Shared @c gate_rv leaves between @c token and @c prov
 * are loaded into a single joint circuit so the indicator's draw
 * and the value's draw share their per-iteration state.
 *
 * @param token  Scalar sub-circuit root.
 * @param n      Number of accepted samples to attempt.
 * @param prov   Conditioning event (defaults to @c gate_one() = no
 *               conditioning).
 *
 * Emits a @c NOTICE when the conditional acceptance rate yields fewer
 * than @c n samples within the @c provsql.rv_mc_samples budget so the
 * caller can choose to widen the budget.
 */
CREATE OR REPLACE FUNCTION rv_sample(
  token UUID, n integer, prov UUID DEFAULT gate_one())
  RETURNS SETOF float8
  AS 'provsql','rv_sample'
  LANGUAGE C VOLATILE PARALLEL SAFE;

/**
 * @brief Resolve an input gate UUID back to its source row
 *
 * Searches every provenance-tracked relation for a row whose
 * <tt>provsql</tt> column equals @p uuid and returns the relation's
 * regclass together with the row encoded as JSONB.  Returns zero
 * rows when @p uuid is not the provenance token of any tracked row,
 * including when it identifies an internal gate (<tt>plus</tt>,
 * <tt>times</tt>, ...) rather than an input.
 *
 * Ordinarily exactly one row is returned, but if the same UUID
 * happens to appear as a <tt>provsql</tt> value in several tracked
 * tables, all matches are returned.
 *
 * @param uuid token to resolve
 */
CREATE OR REPLACE FUNCTION resolve_input(uuid UUID)
  RETURNS TABLE(relation regclass, row_data JSONB) AS
$$
DECLARE
  t RECORD;
  rel regclass;
  rd  JSONB;
  -- ProvSQL's rewriter unconditionally appends a provsql column to the
  -- targetlist of any SELECT reading from a tracked relation; capture and
  -- discard it here rather than disabling the rewriter for the whole call.
  ign UUID;
BEGIN
  FOR t IN
    SELECT c.oid::regclass AS regc
    FROM pg_attribute a
         JOIN pg_class c ON a.attrelid = c.oid
         JOIN pg_namespace ns ON c.relnamespace = ns.oid
         JOIN pg_type ty ON a.atttypid = ty.oid
    WHERE a.attname = 'provsql'
      AND ty.typname = 'uuid'
      AND c.relkind = 'r'
      AND ns.nspname <> 'provsql'
      AND a.attnum > 0
  LOOP
    FOR rel, rd, ign IN
      EXECUTE format(
        'SELECT %L::regclass, to_jsonb(t) - ''provsql'', t.provsql FROM %s AS t WHERE provsql = $1',
        t.regc, t.regc)
      USING uuid
    LOOP
      relation := rel;
      row_data := rd;
      RETURN NEXT;
    END LOOP;
  END LOOP;
END
$$ LANGUAGE plpgsql STABLE;

/** @} */

/** @defgroup agg_token_type Type for the result of aggregate queries
 *
 *  Custom type <tt>agg_token</tt> for a provenance semimodule value, to
 *  be used in attributes that are computed as a result of aggregation.
 *  As for provenance tokens, this is simply a UUID, but this UUID is
 *  displayed in a specific way (as the result of the aggregation
 *  followed by a "(*)") to help with readability.
 *
 *  The text output is controlled by the
 *  <tt>provsql.aggtoken_text_as_uuid</tt> GUC. By default it is off and
 *  the cell renders as <tt>"value (*)"</tt>. When set to on (typical
 *  for UI layers such as ProvSQL Studio), the cell renders as the
 *  underlying UUID instead, so the caller can click through to the
 *  provenance circuit; the value side is then recovered via
 *  <tt>provsql.agg_token_value_text(uuid)</tt>.
 *
 *  @{
 */

CREATE TYPE agg_token;

/** @brief Input function for the agg_token type (parses text representation) */
CREATE OR REPLACE FUNCTION agg_token_in(cstring)
  RETURNS agg_token
  AS 'provsql','agg_token_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Output function for the agg_token type
 *
 * Default: produces the human-friendly @c "value (*)" form, where
 * @c value is the running aggregate state.
 *
 * When the @c provsql.aggtoken_text_as_uuid GUC is on, returns the
 * underlying provenance UUID instead. UI layers (notably ProvSQL
 * Studio) flip this on per session so aggregate cells expose the
 * circuit root UUID for click-through; the @c "value (*)" display
 * string is recovered via @c provsql.agg_token_value_text(uuid).
 *
 * Marked STABLE rather than IMMUTABLE because the chosen output
 * shape now depends on a GUC that the same session can flip at
 * runtime.
 */
CREATE OR REPLACE FUNCTION agg_token_out(agg_token)
  RETURNS cstring
  AS 'provsql','agg_token_out' LANGUAGE C STABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to its text representation */
CREATE OR REPLACE FUNCTION agg_token_cast(agg_token)
  RETURNS text
  AS 'provsql','agg_token_cast' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE agg_token (
  internallength = 117,
  input = agg_token_in,
  output = agg_token_out,
  alignment = char
);

/** @brief Extract the UUID from an agg_token (implicit cast to UUID) */
CREATE OR REPLACE FUNCTION agg_token_uuid(aggtok agg_token)
  RETURNS uuid AS
$$
BEGIN
  RETURN agg_token_cast(aggtok)::uuid;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE PARALLEL SAFE;

/** @brief Implicit PostgreSQL cast from agg_token to UUID (delegates to agg_token_uuid()) */
CREATE CAST (agg_token AS UUID) WITH FUNCTION agg_token_uuid(agg_token) AS IMPLICIT;

/**
 * @brief Recover the @c "value (*)" display string for an aggregation gate
 *
 * Companion helper to the @c provsql.aggtoken_text_as_uuid GUC. With
 * the GUC on, an @c agg_token cell prints as the underlying provenance
 * UUID, which is convenient for tooling that wants to click through to
 * the circuit but loses the human-readable aggregate value. This
 * function takes such a UUID and returns the original @c "value (*)"
 * string by reading the gate's @c extra (set by aggregate evaluation
 * for @c agg gates, and by @c agg_arith_make for the @c arith gates
 * that agg_token arithmetic mints). Returns @c NULL if @p token does
 * not resolve to an @c agg or @c arith gate.
 *
 * @param token UUID of an @c agg gate (typically obtained from an
 *              @c agg_token cell when @c aggtoken_text_as_uuid is on,
 *              or via a manual UUID cast otherwise).
 */
CREATE OR REPLACE FUNCTION agg_token_value_text(token UUID)
  RETURNS text AS
$$
  SELECT CASE
    -- agg gates: extra is set by aggregate evaluation; arith gates
    -- (agg_token arithmetic): extra is recorded by agg_arith_make.
    WHEN provsql.get_gate_type(token) IN ('agg', 'arith')
      THEN provsql.get_extra(token) || ' (*)'
    ELSE NULL
  END;
$$ LANGUAGE sql STABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to numeric (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_numeric(agg_token)
  RETURNS numeric
  AS 'provsql','agg_token_to_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to double precision (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_float8(agg_token)
  RETURNS double precision
  AS 'provsql','agg_token_to_float8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to integer (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_int4(agg_token)
  RETURNS integer
  AS 'provsql','agg_token_to_int4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to bigint (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_int8(agg_token)
  RETURNS bigint
  AS 'provsql','agg_token_to_int8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Cast an agg_token to text (extracts the aggregate value, loses provenance) */
CREATE OR REPLACE FUNCTION agg_token_to_text(agg_token)
  RETURNS text
  AS 'provsql','agg_token_to_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Assignment cast from agg_token to numeric (extracts the scalar
 *  value, dropping provenance).  ASSIGNMENT, not IMPLICIT: provenance-
 *  preserving arithmetic on aggregates is provided by the native
 *  agg_token operators below, so an implicit numeric coercion would only
 *  silently steal `s + 1` away from them (and reroute it differently
 *  depending on whether provsql is in search_path).  Write `s::numeric`
 *  to opt into the lossy scalar. */
CREATE CAST (agg_token AS numeric) WITH FUNCTION agg_token_to_numeric(agg_token) AS ASSIGNMENT;

-- ---------------------------------------------------------------------
-- Arithmetic on aggregates (agg_token)
--
-- Mirrors the random_variable arithmetic surface: the operators build a
-- `gate_arith` over the operand provenance UUIDs (via provenance_arith,
-- info1 = PROVSQL_ARITH_*), so the arithmetic is recorded symbolically
-- in the circuit and can be resolved when a comparison (gate_cmp) over
-- the result is evaluated.  Unlike random_variable (a bare UUID), an
-- agg_token also carries a running scalar value, so each operator
-- additionally computes the resulting value and bundles it back with the
-- new gate.
-- ---------------------------------------------------------------------

/** @brief Running value of an agg_token as numeric, without the
 *  provenance-loss warning the public cast emits (internal use). */
CREATE OR REPLACE FUNCTION agg_token_value(agg_token)
  RETURNS numeric
  AS 'provsql','agg_token_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Bundle a provenance gate UUID with a running value into an
 *  agg_token (inverse of the agg_token_uuid / agg_token_value
 *  accessors). */
CREATE OR REPLACE FUNCTION agg_token_make(tok uuid, val numeric)
  RETURNS agg_token AS
$$
  SELECT format('( %s , %s )', tok::text, val::text)::provsql.agg_token;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

/** @brief Lift a scalar numeric constant into a gate_value leaf and
 *  return its UUID, so it can be a child of a gate_arith (the agg-side
 *  analogue of as_random for random_variable). */
CREATE OR REPLACE FUNCTION agg_value_gate(v numeric)
  RETURNS uuid AS
$$
DECLARE
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', v::text));
BEGIN
  PERFORM provsql.create_gate(token, 'value');
  PERFORM provsql.set_extra(token, v::text);
  RETURN token;
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Mint (or reuse) the gate_arith for an agg_token arithmetic
 *  result and return the agg_token carrying it.
 *
 * Also records the computed scalar in the gate's @c extra -- exactly
 * what aggregate evaluation does for @c agg gates -- so
 * @c agg_token_value_text can recover the @c "value (*)" display from
 * the bare UUID (as ProvSQL Studio does for result cells under
 * @c provsql.aggtoken_text_as_uuid). The gate UUID is deterministic in
 * (op, children), so re-recording the (identical) value is idempotent. */
CREATE OR REPLACE FUNCTION agg_arith_make(op int, children uuid[], val numeric)
  RETURNS agg_token AS
$$
DECLARE
  token uuid := provsql.provenance_arith(op, children);
BEGIN
  PERFORM provsql.set_extra(token, val::text);
  RETURN provsql.agg_token_make(token, val);
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

-- agg_token \<op\> agg_token --------------------------------------------
/** @brief agg_token + agg_token (gate_arith PLUS). */
CREATE OR REPLACE FUNCTION agg_token_plus(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - agg_token (gate_arith MINUS). */
CREATE OR REPLACE FUNCTION agg_token_minus(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * agg_token (gate_arith TIMES). */
CREATE OR REPLACE FUNCTION agg_token_times(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / agg_token (gate_arith DIV). */
CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief Unary -agg_token (gate_arith NEG). */
CREATE OR REPLACE FUNCTION agg_token_neg(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(4, ARRAY[(a)::uuid],
     - provsql.agg_token_value(a)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- agg_token \<op\> numeric ----------------------------------------------
/** @brief agg_token + numeric (gate_arith PLUS, constant lifted to a value gate). */
CREATE OR REPLACE FUNCTION agg_token_plus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) + b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token - numeric. */
CREATE OR REPLACE FUNCTION agg_token_minus_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) - b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token * numeric. */
CREATE OR REPLACE FUNCTION agg_token_times_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) * b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token / numeric. */
CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- numeric \<op\> agg_token ----------------------------------------------
/** @brief numeric + agg_token. */
CREATE OR REPLACE FUNCTION numeric_plus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(0, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a + provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric - agg_token. */
CREATE OR REPLACE FUNCTION numeric_minus_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(2, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a - provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric * agg_token. */
CREATE OR REPLACE FUNCTION numeric_times_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(1, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a * provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief numeric / agg_token. */
CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / provsql.agg_token_value(b)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- Operator declarations -----------------------------------------------
CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_plus,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_minus);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_times, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_div);
CREATE OPERATOR - (RIGHTARG=agg_token, PROCEDURE=agg_token_neg);

CREATE OPERATOR + (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_plus_numeric,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_minus_numeric);
CREATE OPERATOR * (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_times_numeric, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=agg_token, RIGHTARG=numeric, PROCEDURE=agg_token_div_numeric);

CREATE OPERATOR + (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_plus_agg_token,  COMMUTATOR = +);
CREATE OPERATOR - (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_minus_agg_token);
CREATE OPERATOR * (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_times_agg_token, COMMUTATOR = *);
CREATE OPERATOR / (LEFTARG=numeric, RIGHTARG=agg_token, PROCEDURE=numeric_div_agg_token);

/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

/**
 * @brief Condition a discrete aggregate's distribution on an event:
 *        @c "SUM(x) | C".
 *
 * Mirrors @c random_variable_cond for the @c agg_token carrier: returns a
 * conditioned @c agg_token that flows onward, its provenance token wrapped in
 * the composable two-child @c gate_conditioned @c [agg_target, condition]
 * while its running value is preserved.  The moment / support dispatchers
 * unpack it (@c agg_conditioned_target + @c rv_conditioned_prov) and route
 * through the existing @c agg_raw_moment with the condition conjoined into the
 * @c prov argument, so @c expected(SUM(x)|C) / @c variance(SUM(x)|C) report
 * the conditional aggregate distribution.  Nested conditioning folds.
 */
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
 * @brief Placeholder for @c "SUM(x) | (predicate)" on an agg_token.
 *
 * Lets the conditioning event be a natural Boolean predicate (e.g.
 * @c "SUM(x) | (SUM(x) > 5)") instead of a hand-built gate.  Never executes:
 * the planner converts the Boolean operand into a condition gate and emits
 * @c agg_token_cond.
 */
CREATE OR REPLACE FUNCTION agg_token_cond_predicate(
  a agg_token, predicate boolean) RETURNS agg_token AS
$$
BEGIN
  RAISE EXCEPTION 'agg_token | (predicate) must be rewritten by the ProvSQL '
    'planner hook: the right operand must be a Boolean combination of '
    'aggregate / random_variable comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR | (
  LEFTARG   = agg_token,
  RIGHTARG  = boolean,
  PROCEDURE = agg_token_cond_predicate
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

/**
 * @brief Placeholder comparison of agg_token with numeric
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and numeric values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_numeric(a agg_token, b numeric)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-numeric not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of numeric with agg_token
 *
 * Symmetric to agg_token_comp_numeric; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION numeric_comp_agg_token(a numeric, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison numeric-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >,
  NEGATOR    = >=
);
/** @brief SQL operator numeric < agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR < (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >,
  NEGATOR    = >=
);

/** @brief SQL operator agg_token <= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = >=,
  NEGATOR    = >
);
/** @brief SQL operator numeric <= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = >=,
  NEGATOR    = >
);

/** @brief SQL operator agg_token = numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator numeric = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator numeric <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @brief SQL operator agg_token >= numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <=,
  NEGATOR    = <
);
/** @brief SQL operator numeric >= agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR >= (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <=,
  NEGATOR    = <
);

/** @brief SQL operator agg_token > numeric (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = agg_token,
  RIGHTARG   = numeric,
  PROCEDURE  = agg_token_comp_numeric,
  COMMUTATOR = <,
  NEGATOR    = <=
);
/** @brief SQL operator numeric > agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR > (
  LEFTARG    = numeric,
  RIGHTARG   = agg_token,
  PROCEDURE  = numeric_comp_agg_token,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Placeholder comparison of two agg_token values (the diagonal)
 *
 * Never actually called; lets the parser accept agg_token \<op\> agg_token
 * (e.g. sum(x) > sum(y) on materialised tokens), which the ProvSQL
 * rewriter lowers to a gate_cmp at plan time.  Declaring this diagonal
 * also disambiguates `s = s2` (previously "operator is not unique"
 * because both agg_token -> uuid and agg_token -> numeric casts applied).
 */
CREATE OR REPLACE FUNCTION agg_token_comp_agg_token(a agg_token, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token < agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR < (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >, NEGATOR = >=
);
/** @brief SQL operator agg_token <= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = >=, NEGATOR = >
);
/** @brief SQL operator agg_token > agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR > (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <, NEGATOR = <=
);
/** @brief SQL operator agg_token >= agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR >= (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <=, NEGATOR = <
);
/** @brief SQL operator agg_token = agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR = (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = =, NEGATOR = <>
);
/** @brief SQL operator agg_token <> agg_token (placeholder rewritten at plan time) */
CREATE OPERATOR <> (
  LEFTARG=agg_token, RIGHTARG=agg_token, PROCEDURE=agg_token_comp_agg_token,
  COMMUTATOR = <>, NEGATOR = =
);

/**
 * @brief Placeholder comparison of agg_token with text
 *
 * This function is never actually called; it exists so the SQL parser
 * accepts comparison operators between agg_token and text values.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/**
 * @brief Placeholder comparison of text with agg_token
 *
 * Symmetric to agg_token_comp_text; never actually called.
 * The ProvSQL query rewriter replaces these comparisons at plan time.
 */
CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

/** @brief SQL operator agg_token = text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
/** @brief SQL operator text = agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);

/** @brief SQL operator agg_token <> text (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
/** @brief SQL operator text <> agg_token (placeholder rewritten by ProvSQL at plan time) */
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt>: a thin wrapper around a
 *  provenance gate UUID, used to expose continuous probabilistic
 *  c-tables in SQL.  The UUID indexes either a <tt>gate_rv</tt>
 *  (an actual distribution) or a <tt>gate_value</tt> (a
 *  zero-variance constant produced by <tt>provsql.as_random</tt>).
 *  Binary-coercible with <tt>uuid</tt> (same 16-byte layout), so an
 *  <tt>rv</tt>-typed expression flows directly into any function
 *  expecting a uuid at zero runtime cost.
 *
 *  Constructors live in this group: <tt>provsql.normal(μ, σ)</tt>,
 *  <tt>provsql.uniform(a, b)</tt>, <tt>provsql.exponential(λ)</tt>,
 *  <tt>provsql.erlang(k, λ)</tt>, and <tt>provsql.as_random(c)</tt>.
 *  Operator overloads
 *  (<tt>+ - * /</tt> and the six comparators) are defined further
 *  below, alongside direct <tt>rv_cmp_*</tt> UUID constructors for
 *  callers that want a <tt>gate_cmp</tt> token without going through
 *  the planner hook.
 *  @{
 */

CREATE TYPE random_variable;

/** @brief Input function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_in(cstring)
  RETURNS random_variable
  AS 'provsql','random_variable_in' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Output function for the random_variable type */
CREATE OR REPLACE FUNCTION random_variable_out(random_variable)
  RETURNS cstring
  AS 'provsql','random_variable_out' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE random_variable (
  internallength = 16,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = char
);

/** @brief Build a random_variable from a UUID (internal). */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Binary-coercible cast random_variable -> uuid.
 *  A random_variable is byte-for-byte a pg_uuid_t (alignment char,
 *  length 16), so WITHOUT FUNCTION lets PostgreSQL reinterpret the
 *  bytes at zero runtime cost.  The cast is ASSIGNMENT (not IMPLICIT):
 *  an implicit cross-domain cast would silently reroute a comparison
 *  such as `v < w` to `uuid < uuid` (raw byte comparison) whenever
 *  `provsql` is not in search_path, since operators are resolved
 *  through search_path but casts are not.  Demoting to ASSIGNMENT
 *  turns that silent wrong result into a clean parse error.  Passing a
 *  random_variable to a uuid-taking function now needs an explicit
 *  `v::uuid` (function resolution never applies assignment casts). */
CREATE CAST (random_variable AS uuid) WITHOUT FUNCTION AS ASSIGNMENT;
CREATE CAST (uuid AS random_variable) WITHOUT FUNCTION;

/**
 * @brief Internal: true iff @p x is a finite (non-NaN, non-±∞) float8.
 *
 * PostgreSQL's <tt>isnan</tt> is defined for <tt>numeric</tt> only,
 * not for <tt>double precision</tt>; we use the inequality form,
 * which works because PG defines <tt>NaN = NaN</tt> as <tt>TRUE</tt>
 * for floats (so <tt>NaN <> 'NaN'::float8</tt> is <tt>FALSE</tt>).
 */
CREATE OR REPLACE FUNCTION is_finite_float8(x double precision)
  RETURNS bool AS
$$
  SELECT $1 <> 'NaN'::float8 AND $1 <> 'Infinity'::float8 AND $1 <> '-Infinity'::float8;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Construct a normal-distribution random variable
 *
 * Creates a fresh <tt>gate_rv</tt> with @c "normal:μ,σ" stored in
 * the gate's @c extra field, and returns a <tt>random_variable</tt>
 * pointing at it.
 *
 * Validation:
 * - @p mu and @p sigma must be finite (no @c NaN, no @c ±Infinity).
 * - @p sigma must be non-negative.
 * - When @p sigma is zero the distribution degenerates to the Dirac
 *   at @p mu; the call is silently routed through @c as_random(mu),
 *   producing a @c gate_value rather than a zero-variance @c gate_rv.
 *   This keeps the sampler / moment / boundcheck paths free of σ=0
 *   special cases and lets <tt>normal(x, 0)</tt> share its gate with
 *   <tt>as_random(x)</tt>.
 *
 * @warning The <tt>VOLATILE</tt> marking is load-bearing and must
 * not be weakened.  Each call mints a fresh <tt>uuid_generate_v4</tt>
 * token because two calls to <tt>normal(0, 1)</tt> are *independent*
 * random variables; if PostgreSQL were allowed to fold the function
 * (which it would under <tt>STABLE</tt> / <tt>IMMUTABLE</tt>), two
 * calls in the same query would share a UUID and collapse into a
 * single dependent RV, silently breaking the c-table semantics.
 * Same warning applies to @c uniform and @c exponential below.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Normal_distribution">Wikipedia: Normal distribution</a>
 */
CREATE OR REPLACE FUNCTION normal(mu double precision, sigma double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(sigma) THEN
    RAISE EXCEPTION 'provsql.normal: parameters must be finite (got mu=%, sigma=%)', mu, sigma;
  END IF;
  IF sigma < 0 THEN
    RAISE EXCEPTION 'provsql.normal: sigma must be non-negative (got %)', sigma;
  END IF;
  IF sigma = 0 THEN
    RETURN provsql.as_random(mu);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'normal:' || mu || ',' || sigma);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a uniform-distribution random variable on [a, b]
 *
 * Validation:
 * - @p a and @p b must be finite.
 * - @p a must be ≤ @p b (reversed bounds are rejected).
 * - When <tt>a = b</tt> the distribution is the Dirac at @p a; the
 *   call is silently routed through @c as_random(a) for the same
 *   reason as @c normal with @p sigma = 0.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Continuous_uniform_distribution">Wikipedia: Continuous uniform distribution</a>
 */
CREATE OR REPLACE FUNCTION uniform(a double precision, b double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(a) OR NOT provsql.is_finite_float8(b) THEN
    RAISE EXCEPTION 'provsql.uniform: bounds must be finite (got a=%, b=%)', a, b;
  END IF;
  IF a > b THEN
    RAISE EXCEPTION 'provsql.uniform: a must be <= b (got a=%, b=%)', a, b;
  END IF;
  IF a = b THEN
    RETURN provsql.as_random(a);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'uniform:' || a || ',' || b);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct an exponential-distribution random variable with rate λ
 *
 * Validation:
 * - @p lambda must be finite and strictly positive.  No degenerate
 *   form exists for the exponential distribution, so there is no
 *   silent route through @c as_random.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Exponential_distribution">Wikipedia: Exponential distribution</a>
 */
CREATE OR REPLACE FUNCTION exponential(lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be strictly positive (got %)', lambda;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'exponential:' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct an Erlang-distribution random variable, sum of
 *        @p k i.i.d. exponentials with shared rate @p lambda
 *
 * The Erlang distribution is the sum of @p k independent
 * <tt>Exp(λ)</tt> random variables (equivalently the gamma with
 * integer shape).  It is the natural closure of i.i.d.
 * exponentials under addition, and is materialised here as a single
 * <tt>gate_rv</tt> so the analytic CDF and closed-form moments fire
 * directly (rather than the sampler having to draw and sum @p k
 * exponential leaves per Monte-Carlo iteration).
 *
 * Validation:
 * - @p k must be ≥ 1.  The degenerate @c k=1 case is silently routed
 *   through @c exponential so <tt>erlang(1, λ)</tt> shares its gate
 *   with <tt>exponential(λ)</tt>.
 * - @p lambda must be finite and strictly positive.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Erlang_distribution">Wikipedia: Erlang distribution</a>
 */
CREATE OR REPLACE FUNCTION erlang(k integer, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF k < 1 THEN
    RAISE EXCEPTION 'provsql.erlang: k must be >= 1 (got %)', k;
  END IF;
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be strictly positive (got %)', lambda;
  END IF;
  IF k = 1 THEN
    RETURN provsql.exponential(lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'erlang:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a probabilistic-mixture random variable.
 *
 * Returns a @c random_variable whose distribution is a Bernoulli
 * mixture of two scalar RV roots: with probability <tt>P(p = true)</tt>
 * the mixture samples @p x, with the complementary probability it
 * samples @p y.  The mixing token @p p is a @c gate_input Bernoulli
 * whose probability has been pinned with @c set_prob, and the same
 * @p p can be shared with other branches of the circuit -- the
 * Monte-Carlo sampler's per-iteration cache couples every reference
 * to the same draw, so users can build joint conditional structures
 * (e.g. <tt>mixture(p, X1, Y1) + mixture(p, X2, Y2)</tt> samples
 * X1 + X2 with prob π and Y1 + Y2 with prob 1-π).
 *
 * @p x and @p y may be any scalar RV root: a base @c gate_rv
 * (@c normal / @c uniform / @c exponential / @c erlang), a
 * @c gate_value Dirac (@c as_random), a @c gate_arith expression, or
 * another @c mixture.  N-ary mixtures are built by composition --
 * <tt>mixture(p1, A, mixture(p2, B, C))</tt> realises a 3-component
 * mixture with effective weights <tt>π1, (1-π1)·π2, (1-π1)·(1-π2)</tt>.
 *
 * Validation:
 * - @p p must point to a Boolean gate (@c input, @c mulinput,
 *   @c update, @c plus, @c times, @c monus, @c project, @c eq,
 *   @c cmp, @c zero, @c one).  Compound Boolean gates derive their
 *   probability from their atoms via the active probability-evaluation
 *   method; a bare @c gate_input's probability is whatever @c set_prob
 *   pinned (@c set_prob is responsible for keeping it in [0, 1]).
 * - @p x and @p y must be scalar RV roots; aggregate / Boolean roots
 *   are rejected at construction.
 *
 * Two calls to @c mixture with the same @c (p, x, y) operands collapse
 * to the same @c gate_mixture node by v5-hash, exactly like
 * @c arith(PLUS, X, Y).  Draw independence is controlled by @p p:
 * sharing @p p couples branch selection across consumers via the
 * sampler's @c bool_cache_; minting independent Bernoullis (e.g. via
 * the @c mixture(p_value, …) overload) decouples them.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Mixture_distribution">Wikipedia: Mixture distribution</a>
 */
CREATE OR REPLACE FUNCTION mixture(
  p uuid, x random_variable, y random_variable)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
  p_kind provsql.provenance_gate;
  x_uuid uuid;
  y_uuid uuid;
  x_kind provsql.provenance_gate;
  y_kind provsql.provenance_gate;
BEGIN
  p_kind := provsql.get_gate_type(p);
  IF p_kind NOT IN ('input','mulinput','update',
                    'plus','times','monus',
                    'project','eq','cmp',
                    'zero','one') THEN
    RAISE EXCEPTION 'provsql.mixture: p must be a Boolean gate '
                    '(input/mulinput/update/plus/times/monus/project/eq/cmp/zero/one), got %', p_kind;
  END IF;

  x_uuid := (x)::uuid;
  y_uuid := (y)::uuid;
  x_kind := provsql.get_gate_type(x_uuid);
  y_kind := provsql.get_gate_type(y_uuid);
  IF x_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: x must be a scalar RV root (rv / value / arith / mixture), got %', x_kind;
  END IF;
  IF y_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: y must be a scalar RV root (rv / value / arith / mixture), got %', y_kind;
  END IF;

  token := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(),
    concat('mixture', p, x_uuid, y_uuid));
  PERFORM provsql.create_gate(token, 'mixture', ARRAY[p, x_uuid, y_uuid]);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE;

/**
 * @brief Ad-hoc mixture constructor that mints a fresh anonymous
 *        @c gate_input Bernoulli with probability @p p_value.
 *
 * Sugar over the @c mixture(uuid, x, y) form: when the caller doesn't
 * care about reusing the Bernoulli token elsewhere in the circuit
 * (which is the common case &ndash; "give me a 0.3 / 0.7 weighted GMM,
 * I don't need to share the coin"), this overload creates the
 * underlying @c gate_input on the fly with a fresh
 * @c uuid_generate_v4() token, pins @p p_value via @c set_prob, and
 * threads everything into the uuid-keyed constructor.
 *
 * Each call mints a NEW Bernoulli, so two calls to
 * <tt>mixture(0.5, X, Y)</tt> are *independent* mixtures whose branch
 * selections are uncorrelated.  When coupling is desired (e.g. two
 * mixtures sharing a coin), use the @c mixture(uuid, x, y) form with a
 * user-managed @c gate_input token.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing for the same reason as
 * @ref normal and the other RV constructors -- folding under
 * @c STABLE / @c IMMUTABLE would collapse two independent draws into
 * one shared gate.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Mixture_distribution">Wikipedia: Mixture distribution</a>
 */
CREATE OR REPLACE FUNCTION mixture(
  p_value double precision,
  x random_variable,
  y random_variable)
  RETURNS random_variable AS
$$
DECLARE
  p_token uuid;
BEGIN
  IF p_value IS NULL OR p_value <> p_value OR p_value < 0 OR p_value > 1 THEN
    RAISE EXCEPTION 'provsql.mixture: probability must be in [0,1] (got %)', p_value;
  END IF;
  p_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(p_token, 'input');
  PERFORM provsql.set_prob(p_token, p_value);
  RETURN provsql.mixture(p_token, x, y);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Categorical-RV constructor over explicit (probabilities,
 *        values) arrays.
 *
 * Builds a categorical-form @c gate_mixture directly: a fresh
 * @c gate_input "key" anchor and one @c gate_mulinput per outcome with
 * positive mass, all sharing the key.  The wires
 * <tt>[key, mul_1, ..., mul_n]</tt> are what downstream evaluators
 * (@c Expectation, @c MonteCarloSampler, @c AnalyticEvaluator,
 * @c RangeCheck) recognise via @c isCategoricalMixture and treat as a
 * scalar RV with the categorical distribution @p probs over
 * @p outcomes.
 *
 * Validation:
 * - @p probs and @p outcomes must be non-null, same length, length &ge; 1.
 * - Each @c probs[i] must be finite, in <tt>[0, 1]</tt>, and the array
 *   must sum to 1 within @c 1e-9.
 * - Each @c outcomes[i] must be finite.
 *
 * Each call mints a fresh key gate and a fresh set of mulinputs, so
 * two calls to @c categorical with the same arrays are *independent*
 * categorical RVs.  The marking is @c VOLATILE accordingly.
 *
 * Degenerate case: a categorical with exactly one positive-mass
 * outcome reduces to @c as_random(v) at construction (the block would
 * just be a single mulinput, which is operationally a Dirac point
 * mass).  Two such calls share the @c gate_value UUID via the v5
 * convention @c as_random already uses.
 *
 * @sa @c mixture for the Bernoulli-weighted choice constructor.
 * @sa <a href="https://en.wikipedia.org/wiki/Categorical_distribution">Wikipedia: Categorical distribution</a>
 */
CREATE OR REPLACE FUNCTION categorical(
  probs    double precision[],
  outcomes double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  p_sum double precision := 0.0;
  i integer;
  key_token uuid;
  mix_token uuid;
  mul_token uuid;
  mul_tokens uuid[] := ARRAY[]::uuid[];
  mix_wires  uuid[];
  pi_i double precision;
  vi_i double precision;
BEGIN
  IF probs IS NULL OR outcomes IS NULL THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must be non-null';
  END IF;
  n := array_length(probs, 1);
  IF n IS NULL OR n < 1 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must be non-empty';
  END IF;
  IF array_length(outcomes, 1) <> n THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must have the same length (got % and %)',
      n, array_length(outcomes, 1);
  END IF;

  FOR i IN 1..n LOOP
    pi_i := probs[i];
    vi_i := outcomes[i];
    -- PostgreSQL diverges from IEEE 754: NaN = NaN is TRUE there, so
    -- the canonical x <> x NaN test doesn't fire.  Compare against the
    -- literal 'NaN'::float8 instead, and reject ±Infinity for outcomes
    -- explicitly.
    IF pi_i IS NULL OR pi_i = 'NaN'::float8 OR pi_i < 0 OR pi_i > 1 THEN
      RAISE EXCEPTION 'provsql.categorical: probs[%] must be in [0,1] (got %)', i, pi_i;
    END IF;
    IF vi_i IS NULL OR vi_i = 'NaN'::float8
       OR vi_i = 'Infinity'::float8 OR vi_i = '-Infinity'::float8 THEN
      RAISE EXCEPTION 'provsql.categorical: outcomes[%] must be finite (got %)', i, vi_i;
    END IF;
    p_sum := p_sum + pi_i;
  END LOOP;
  IF abs(p_sum - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must sum to 1 within 1e-9 (got %)', p_sum;
  END IF;

  -- Degenerate case: exactly one positive-mass outcome (the rest are
  -- zero).  The "categorical" is then a Dirac point mass; skip the
  -- block-allocation entirely and return @c as_random(v), which yields
  -- a shared, v5-keyed gate_value -- exactly what downstream
  -- evaluators (rv_moment, AnalyticEvaluator, rv_support) treat
  -- specially.  Saves a key gate and a mulinput per call, and lets
  -- two calls to @c categorical({1.0}, {v}) collide on the same
  -- gate_value UUID instead of producing distinct anonymous blocks.
  DECLARE
    nb_positive integer := 0;
    only_idx    integer := 0;
  BEGIN
    FOR i IN 1..n LOOP
      IF probs[i] > 0.0 THEN
        nb_positive := nb_positive + 1;
        only_idx := i;
      END IF;
    END LOOP;
    IF nb_positive = 1 THEN
      RETURN provsql.as_random(outcomes[only_idx]);
    END IF;
  END;

  -- Mint the block's key anchor.  Probability 1.0 matches the
  -- joint-table convention: the categorical mass lives on the
  -- mulinputs, the key just identifies the block.
  key_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(key_token, 'input');
  PERFORM provsql.set_prob(key_token, 1.0);

  -- One mulinput per positive-probability outcome.  Zero-probability
  -- entries contribute no mass and are skipped: the gate_mixture's
  -- wire vector is otherwise polluted with no-op leaves.
  FOR i IN 1..n LOOP
    pi_i := probs[i];
    IF pi_i <= 0.0 THEN CONTINUE; END IF;
    mul_token := public.uuid_generate_v4();
    PERFORM provsql.create_gate(mul_token, 'mulinput', ARRAY[key_token]);
    PERFORM provsql.set_prob(mul_token, pi_i);
    PERFORM provsql.set_infos(mul_token, (i - 1));
    PERFORM provsql.set_extra(mul_token, outcomes[i]::text);
    mul_tokens := mul_tokens || mul_token;
  END LOOP;

  mix_wires := ARRAY[key_token] || mul_tokens;
  mix_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(mix_token, 'mixture', mix_wires);
  RETURN provsql.random_variable_make(mix_token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Lift a deterministic constant into a random_variable
 *
 * Creates a <tt>gate_value</tt> carrying the constant's text form so
 * that comparisons against a <tt>random_variable</tt> column produce
 * the same circuit shape regardless of whether the operand is an
 * actual RV or a literal constant.
 *
 * Marked <tt>IMMUTABLE</tt>: the gate UUID is derived deterministically
 * from the constant via the same v5 convention as <tt>provenance_semimod</tt>'s
 * inline value gate (<tt>concat('value', CAST(c AS VARCHAR))</tt>), so
 * <tt>as_random(2)</tt> always resolves to the same gate, and any other
 * code path that already creates a value gate for the same constant
 * (e.g. <tt>provenance_semimod</tt>) shares the UUID.
 * <tt>create_gate</tt> is idempotent on already-mapped tokens, so
 * repeat invocations are harmless.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Degenerate_distribution">Wikipedia: Degenerate distribution (Dirac point mass)</a>
 */
CREATE OR REPLACE FUNCTION as_random(c double precision)
  RETURNS random_variable AS
$$
DECLARE
  -- Canonicalise -0.0 to +0.0: IEEE 754 defines x + 0.0 = +0.0 for
  -- both signed zeros, and is identity for finite, NaN, and ±Infinity.
  -- Without this, as_random(-0.0) and as_random(+0.0) would produce
  -- different gate UUIDs (their CAST AS VARCHAR text representations
  -- differ: '-0' vs '0') even though they denote the same constant.
  c_canon double precision := c + 0.0;
  c_text varchar := CAST(c_canon AS VARCHAR);
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', c_text));
BEGIN
  PERFORM provsql.create_gate(token, 'value');
  PERFORM provsql.set_extra(token, c_text);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE;

/**
 * @brief Implicit cast double precision -> random_variable (lifts a
 *        scalar literal to a constant RV).
 *
 * Lets users write <tt>WHERE reading > 2.5::float8</tt> instead of
 * <tt>WHERE reading > provsql.as_random(2.5)</tt>; the planner-hook
 * rewriter then sees a uniform <tt>random_variable</tt> on both sides.
 * Sibling casts below cover @c integer and @c numeric literals so
 * plain <tt>WHERE reading > 2</tt> and <tt>WHERE reading > 2.5</tt>
 * also work; PostgreSQL's operator resolution does not chain casts
 * across more than one step, so each numeric-source type needs its
 * own direct cast.
 */
CREATE CAST (double precision AS random_variable)
  WITH FUNCTION as_random(double precision) AS IMPLICIT;

/** @brief @c as_random for @c integer (delegates to the @c float8 form). */
CREATE OR REPLACE FUNCTION as_random(c integer)
  RETURNS random_variable AS
$$ SELECT provsql.as_random(c::double precision); $$
LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE;

/** @brief @c as_random for @c numeric (delegates to the @c float8 form). */
CREATE OR REPLACE FUNCTION as_random(c numeric)
  RETURNS random_variable AS
$$ SELECT provsql.as_random(c::double precision); $$
LANGUAGE sql STRICT IMMUTABLE PARALLEL SAFE;

/** @brief Implicit cast integer -> random_variable. */
CREATE CAST (integer AS random_variable)
  WITH FUNCTION as_random(integer) AS IMPLICIT;

/** @brief Implicit cast numeric -> random_variable. */
CREATE CAST (numeric AS random_variable)
  WITH FUNCTION as_random(numeric) AS IMPLICIT;

/**
 * @name Arithmetic and comparison on random_variable
 *
 * Each binary operator below is declared on @c (random_variable,
 * random_variable) only; mixed shapes such as <tt>rv + 2</tt> or
 * <tt>2.5 > rv</tt> resolve through the implicit casts from
 * @c integer / @c numeric / @c double @c precision to
 * @c random_variable declared above.  This avoids the resolution
 * ambiguity that would arise if both <tt>(rv, numeric)</tt> and
 * <tt>(rv, rv)</tt> overloads were declared while implicit casts also
 * existed.
 *
 * Arithmetic operators build a @c gate_arith via @c provenance_arith
 * and return a new @c random_variable wrapping its UUID.
 *
 * Comparison operators are placeholders that return @c boolean and
 * raise if executed -- the @c boolean return type is required so that
 * PostgreSQL accepts <tt>WHERE rv > 2</tt> at parse-analyze.  The
 * planner hook intercepts every such @c OpExpr (matched by
 * @c opfuncid against @c constants_t::OID_FUNCTION_RV_CMP) and rewrites
 * it into a @c provenance_cmp call whose UUID is conjoined into the
 * tuple's @c provsql column via @c provenance_times.  Code that needs
 * a @c gate_cmp UUID directly (without going through the planner hook)
 * uses the @c rv_cmp_* family below, which call @c provenance_cmp
 * with the matching float8-comparator OID.
 *
 * @{
 */

/** @brief @c random_variable + @c random_variable (gate_arith PLUS). */
CREATE OR REPLACE FUNCTION random_variable_plus(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      0,  -- PROVSQL_ARITH_PLUS
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable - @c random_variable (gate_arith MINUS). */
CREATE OR REPLACE FUNCTION random_variable_minus(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      2,  -- PROVSQL_ARITH_MINUS
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable * @c random_variable (gate_arith TIMES). */
CREATE OR REPLACE FUNCTION random_variable_times(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      1,  -- PROVSQL_ARITH_TIMES
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable / @c random_variable (gate_arith DIV). */
CREATE OR REPLACE FUNCTION random_variable_div(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- PROVSQL_ARITH_DIV
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Unary @c -random_variable (gate_arith NEG). */
CREATE OR REPLACE FUNCTION random_variable_neg(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      4,  -- PROVSQL_ARITH_NEG
      ARRAY[(a)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Internal helper: float8-comparator OID for a given symbol.
 *
 * Wraps the @c '&lt;sym&gt;(double precision,double precision)'::regoperator
 * lookup so the per-comparator functions read uniformly.  Marked
 * @c IMMUTABLE because the resolved OID is fixed at catalog level
 * (the float8 comparators are core PG and never re-installed).
 */
CREATE OR REPLACE FUNCTION random_variable_cmp_oid(sym text)
  RETURNS oid AS
$$
  SELECT (sym || '(double precision,double precision)')::regoperator::oid;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/* The six @c random_variable_{lt,le,eq,ne,ge,gt} functions below are
 * boolean placeholders -- they exist only so the @c (rv, rv) operators
 * can be declared at all (PostgreSQL needs a procedure to bind to the
 * operator definition, and a procedure returning anything but @c boolean
 * would be rejected by parse-analyze in a WHERE position).  They MUST
 * NOT be invoked directly: the planner hook in @c src/provsql.c
 * intercepts every @c OpExpr whose @c opfuncid matches one of these and
 * rewrites it into a @c provenance_cmp() call against the row's
 * provenance.  If the executor ever reaches one of these, it means the
 * planner hook was bypassed (e.g. @c provsql.active was off), in which
 * case raising is the right behaviour. */

/** @brief Placeholder body shared by every <tt>random_variable_*</tt>
 *  comparison procedure.  Raises with a uniform message. */
CREATE OR REPLACE FUNCTION random_variable_cmp_placeholder(
  a random_variable, b random_variable)
  RETURNS boolean AS
$$
BEGIN
  RAISE EXCEPTION 'random_variable comparison must be rewritten by the '
                  'ProvSQL planner hook (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_lt(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_le(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_eq(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_ne(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_ge(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_gt(
  a random_variable, b random_variable) RETURNS boolean AS
$$ SELECT provsql.random_variable_cmp_placeholder(a, b); $$
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/* Direct UUID constructors -- used by tests and any caller that wants
 * a @c gate_cmp without going through the planner hook (e.g. building
 * a circuit fragment in a SELECT list).  Each delegates to
 * @c provenance_cmp with the matching float8-comparator OID. */

/** @brief Build a @c gate_cmp for <tt>a &lt; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_lt(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &le; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_le(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a = b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_eq(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &lt;&gt; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_ne(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('<>'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &ge; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_ge(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('>='),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &gt; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_gt(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    (a)::uuid,
    provsql.random_variable_cmp_oid('>'),
    (b)::uuid);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_plus,
  COMMUTATOR = +
);

CREATE OPERATOR - (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_minus
);

CREATE OPERATOR * (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_times,
  COMMUTATOR = *
);

CREATE OPERATOR / (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_div
);

/** @brief Prefix unary minus on @c random_variable. */
CREATE OPERATOR - (
  RIGHTARG  = random_variable,
  PROCEDURE = random_variable_neg
);

CREATE OPERATOR < (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_lt,
  COMMUTATOR = >,
  NEGATOR    = >=
);

CREATE OPERATOR <= (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_le,
  COMMUTATOR = >=,
  NEGATOR    = >
);

CREATE OPERATOR = (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_eq,
  COMMUTATOR = =,
  NEGATOR    = <>
);

CREATE OPERATOR <> (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_ne,
  COMMUTATOR = <>,
  NEGATOR    = =
);

CREATE OPERATOR >= (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_ge,
  COMMUTATOR = <=,
  NEGATOR    = <
);

CREATE OPERATOR > (
  LEFTARG    = random_variable,
  RIGHTARG   = random_variable,
  PROCEDURE  = random_variable_gt,
  COMMUTATOR = <,
  NEGATOR    = <=
);

/**
 * @brief Condition a random variable on an event: @c "X | C".
 *
 * Returns a conditioned distribution that flows onward like any other
 * @c random_variable: it can be stored, re-conditioned, and queried with
 * @c expected / @c variance / @c moment / @c support, which then report the
 * conditional distribution.  @p cond is a Boolean-event provenance token,
 * typically a comparison over the variable itself (@c "X | rv_cmp_gt(X,
 * as_random(3))" -- a truncation) or any external event.
 *
 * Unlike the uuid carrier's terminal @c cond, the random-variable form is a
 * composable two-child @c gate_conditioned @c [target, condition]: the moment
 * / support dispatchers unpack it and route through the existing conditional
 * evaluator (@c rv_moment over the joint of the target and the condition).
 * Nested conditioning folds: @c "(X|A)|B = X|(A∧B)".
 */
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
 * @brief Placeholder for @c "X | (predicate)" -- conditioning a random
 *        variable on a Boolean comparison written naturally.
 *
 * Lets one write @c "X | (X > 3)" instead of
 * @c "X | rv_cmp_gt(X, as_random(3))".  Never executes: the ProvSQL planner
 * hook rewrites the Boolean operand (a combination of random_variable
 * comparisons) into the corresponding condition gate and emits
 * @c random_variable_cond.  Reaching it at runtime means the rewriter was
 * inactive or the predicate was not a random_variable comparison.
 */
CREATE OR REPLACE FUNCTION random_variable_cond_predicate(
  rv random_variable, predicate boolean) RETURNS random_variable AS
$$
BEGIN
  RAISE EXCEPTION 'random_variable | (predicate) must be rewritten by the '
    'ProvSQL planner hook: the right operand must be a Boolean combination '
    'of random_variable comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR | (
  LEFTARG   = random_variable,
  RIGHTARG  = boolean,
  PROCEDURE = random_variable_cond_predicate
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

/** @} */

/**
 * @name Aggregates over random_variable
 *
 * Phase 1 of the SUM-over-RV story: an overload of the standard
 * @c sum aggregate that takes a @c random_variable per row and returns
 * the @c random_variable representing the (provenance-weighted) sum.
 * Lives in the @c provsql schema so a @c sum(random_variable) call
 * resolves to it without colliding with the built-in numeric @c sum
 * overloads in @c pg_catalog.
 *
 * Direct calls outside a provenance-tracked query treat each row's
 * contribution unconditionally (no per-row Boolean selector).  When
 * the planner hook sees a @c provsql.sum @c Aggref over a
 * provenance-tracked query, it wraps the per-row argument @c x in
 * <tt>provsql.mixture(prov_token, x, provsql.as_random(0))</tt> so the
 * aggregate's effective semantics become
 * @f$\mathrm{SUM}(x) = \sum_i \mathbf{1}\{\varphi_i\} \cdot X_i@f$,
 * the natural extension of semimodule-provenance to RV-valued M.
 *
 * The internal state is the array of UUIDs of the per-row mixtures.
 * The final function builds a single @c gate_arith @c PLUS over them
 * (or returns @c as_random(0) for an empty group, the additive
 * identity).  Sharing on @c provenance_arith's v5 hash means two
 * @c sum invocations over the same set of rows collide on the same
 * gate.
 *
 * @{
 */

/**
 * @brief Per-row helper: wrap an RV in @c mixture(prov, rv, as_random(0)).
 *
 * Internal helper used by the planner-hook rewriter to lift a
 * @c sum(random_variable) argument into its provenance-aware form.
 * Encodes one row's contribution to the SUM as a Bernoulli mixture
 * over the row's provenance: with probability @c P(prov) the mixture
 * samples @c rv, otherwise it samples the additive identity
 * @c as_random(0).  Exposed as a regular SQL function so the planner
 * can construct a @c FuncExpr by name without needing to disambiguate
 * @c mixture / @c as_random overloads at OID-lookup time.
 */
CREATE OR REPLACE FUNCTION rv_aggregate_semimod(
  prov uuid, rv random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.mixture(prov, rv, provsql.as_random(0::double precision));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief State-transition function for @c sum(random_variable).
 *
 * Appends the input RV's UUID to the running array.  NULL inputs are
 * skipped (matching standard SUM semantics).  The aggregate's INITCOND
 * is @c '{}' so the FINALFUNC always runs even on an empty group, which
 * is what lets us return @c as_random(0) (the additive identity) for
 * an empty SUM rather than NULL.
 */
CREATE OR REPLACE FUNCTION sum_rv_sfunc(
  state uuid[], rv random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN rv IS NULL THEN state
    ELSE array_append(state, (rv)::uuid)
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Final function for @c sum(random_variable): build a
 *        @c gate_arith PLUS root.
 *
 * Empty group (@c state = @c '{}'): return @c as_random(0), the
 * additive identity, so SUM over zero rows is the deterministic
 * scalar 0 -- matches the agg_token convention in @c agg_raw_moment.
 *
 * Singleton group: return the single child directly without minting a
 * useless single-child @c gate_arith.
 *
 * Otherwise: build @c gate_arith(PLUS, state) via @c provenance_arith.
 */
CREATE OR REPLACE FUNCTION sum_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  arith_token uuid;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN provsql.as_random(0::double precision);
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  arith_token := provsql.provenance_arith(0, state);  -- 0 = PROVSQL_ARITH_PLUS
  RETURN provsql.random_variable_make(arith_token);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE AGGREGATE sum(random_variable) (
  SFUNC     = sum_rv_sfunc,
  STYPE     = uuid[],
  INITCOND  = '{}',
  FINALFUNC = sum_rv_ffunc
);

/**
 * @brief Final function for @c avg(random_variable).
 *
 * Builds the natural lift of @c "AVG = SUM / COUNT" into the
 * @c random_variable algebra:
 * @f[
 *   \mathrm{AVG}(x) \;=\; \frac{\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i}
 *                                {\sum_i \mathbf{1}\{\varphi_i\}}
 * @f]
 * realised as @c gate_arith(DIV, num, denom) where @c num is the
 * @c sum(random_variable) gate over the per-row mixtures and @c denom
 * is the @c sum(random_variable) gate over the same provenance gates
 * weighted by a per-row @c as_random(1) -- exactly the SQL pattern
 * "@c sum(x) @c / @c sum(as_random(1))" emitted as a single
 * @c random_variable token.
 *
 * Reuses @c sum_rv_sfunc as the state-transition function so the
 * array of per-row UUIDs is collected identically to
 * @c sum(random_variable).  In a provenance-tracked query the
 * planner-hook rewriter routes RV-returning aggregates through
 * @c make_rv_aggregate_expression, which wraps each per-row argument
 * in @c mixture(prov_i, x_i, as_random(0)); the FFUNC then recovers
 * @c prov_i from each mixture's first child to construct the matching
 * @c mixture(prov_i, as_random(1), as_random(0)) for the denominator.
 * Outside a tracked query the per-row UUIDs are plain RV roots, in
 * which case each row contributes an unconditional @c as_random(1)
 * to the denominator -- the natural extension of "no provenance =
 * every row counts" used elsewhere in the extension.
 *
 * Empty group: returns @c NULL, matching the standard SQL @c AVG
 * convention.  This differs from @c sum(random_variable), which
 * returns the additive identity @c as_random(0) for an empty group;
 * for AVG the multiplicative identity is not the right answer and
 * the caller has no way to disambiguate "0 rows" from "rows that
 * sum to 0".
 */
CREATE OR REPLACE FUNCTION avg_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  num_token uuid;
  denom_token uuid;
  denom_state uuid[] := '{}';
  one_uuid uuid;
  gtype provsql.provenance_gate;
  children uuid[];
  prov_i uuid;
BEGIN
  IF state IS NULL THEN
    RETURN NULL;
  END IF;
  n := array_length(state, 1);
  IF n IS NULL THEN
    RETURN NULL;
  END IF;

  one_uuid := (
                provsql.as_random(1::double precision))::uuid;

  FOR i IN 1..n LOOP
    gtype := provsql.get_gate_type(state[i]);
    IF gtype = 'mixture'::provsql.provenance_gate THEN
      children := provsql.get_children(state[i]);
      prov_i := children[1];
      denom_state := array_append(
        denom_state,
        (
          provsql.rv_aggregate_semimod(
            prov_i, provsql.as_random(1::double precision)))::uuid);
    ELSE
      denom_state := array_append(denom_state, one_uuid);
    END IF;
  END LOOP;

  IF n = 1 THEN
    num_token := state[1];
    denom_token := denom_state[1];
  ELSE
    num_token := provsql.provenance_arith(0, state);          -- 0 = PLUS
    denom_token := provsql.provenance_arith(0, denom_state);  -- 0 = PLUS
  END IF;

  RETURN provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- 3 = PROVSQL_ARITH_DIV
      ARRAY[num_token, denom_token]));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE AGGREGATE avg(random_variable) (
  SFUNC     = sum_rv_sfunc,
  STYPE     = uuid[],
  INITCOND  = '{}',
  FINALFUNC = avg_rv_ffunc
);

/**
 * @brief Final function for @c product(random_variable).
 *
 * Multiplicative analogue of @c sum(random_variable):
 * @f[
 *   \mathrm{PRODUCT}(x) \;=\; \prod_i \big(\mathbf{1}\{\varphi_i\} \cdot X_i
 *                                          + \mathbf{1}\{\neg\varphi_i\} \cdot 1\big)
 *                       \;=\; \prod_{i : \varphi_i} X_i
 * @f]
 * realised as @c gate_arith(TIMES, mixtures) over per-row contributions
 * whose @em else-branch is @c as_random(1) (the multiplicative
 * identity), so rows whose provenance is false contribute @c 1 to the
 * product instead of @c 0.
 *
 * The C-side wrap shared with @c sum / @c avg always builds
 * @c mixture(prov_i, X_i, as_random(0)); the PRODUCT FFUNC patches each
 * mixture's else-branch to @c as_random(1) by reconstructing the
 * mixture with the corrected else-arg.  Going through
 * @c provsql.mixture (rather than @c create_gate directly) keeps the
 * gate v5-hash consistent with any other mixture sharing the same
 * @c (prov_i, X_i, as_random(1)) triple.
 *
 * Reuses @c sum_rv_sfunc as the state-transition function.  Empty
 * group: returns the multiplicative identity @c as_random(1) -- the
 * natural counterpart to @c sum(random_variable)'s empty-group
 * @c as_random(0).
 *
 * Singleton group: returns the single patched child directly without
 * minting a useless single-child @c gate_arith TIMES root.
 *
 * Direct (untracked) call: state entries are raw RV uuids rather than
 * mixtures; pass them through unchanged so PRODUCT degenerates to the
 * straight RV product over all rows, the natural "no provenance =
 * every row counts" behaviour.
 */
CREATE OR REPLACE FUNCTION product_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  prod_state uuid[] := '{}';
  one_rv provsql.random_variable;
  gtype provsql.provenance_gate;
  children uuid[];
  prov_i uuid;
  x_uuid uuid;
BEGIN
  one_rv := provsql.as_random(1::double precision);

  IF state IS NULL THEN
    RETURN one_rv;
  END IF;
  n := array_length(state, 1);
  IF n IS NULL THEN
    RETURN one_rv;
  END IF;

  FOR i IN 1..n LOOP
    gtype := provsql.get_gate_type(state[i]);
    IF gtype = 'mixture'::provsql.provenance_gate THEN
      children := provsql.get_children(state[i]);
      prov_i := children[1];
      x_uuid := children[2];
      prod_state := array_append(
        prod_state,
        (
          provsql.mixture(
            prov_i,
            provsql.random_variable_make(x_uuid),
            one_rv))::uuid);
    ELSE
      prod_state := array_append(prod_state, state[i]);
    END IF;
  END LOOP;

  IF n = 1 THEN
    RETURN provsql.random_variable_make(prod_state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(1, prod_state));  -- 1 = PROVSQL_ARITH_TIMES
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE AGGREGATE product(random_variable) (
  SFUNC     = sum_rv_sfunc,
  STYPE     = uuid[],
  INITCOND  = '{}',
  FINALFUNC = product_rv_ffunc
);

/** @} */

/** @} */

/** @defgroup aggregate_provenance Aggregate provenance
 *  Functions for building and evaluating aggregate (GROUP BY) provenance,
 *  including the δ-semiring operator and semimodule multiplication.
 *  @{
 */

/**
 * @brief Create a δ-semiring gate wrapping a provenance token
 *
 * Used internally for aggregate provenance. Returns the token unchanged
 * if it is gate_zero() or gate_one(), and gate_one() if the token is NULL.
 */
CREATE OR REPLACE FUNCTION provenance_delta
  (token UUID)
  RETURNS UUID AS
$$
DECLARE
  delta_token uuid;
BEGIN
  IF token = gate_zero() OR token = gate_one() THEN
    return token;
  END IF;

  IF token IS NULL THEN
    return gate_one();
  END IF;

  delta_token:=uuid_generate_v5(uuid_ns_provsql(),concat('delta',token));

  PERFORM create_gate(delta_token,'delta',ARRAY[token::uuid]);

  RETURN delta_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

/**
 * @brief Build an aggregate provenance gate from grouped tokens
 *
 * Called internally by the query rewriter for GROUP BY queries.
 * Creates an agg gate linking all contributing tokens and records
 * the aggregate function OID and the computed scalar value.
 *
 * @param aggfnoid OID of the SQL aggregate function
 * @param aggtype OID of the aggregate result type
 * @param val computed aggregate value
 * @param tokens array of provenance tokens being aggregated
 * @param is_scalar true for a scalar (no GROUP BY) aggregation, whose
 *        output row exists even when no tuple is present; stored in the
 *        high bit of info2
 */
CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[],
    is_scalar boolean DEFAULT false)
  RETURNS agg_token AS
$$
DECLARE
  c INTEGER;
  agg_tok uuid;
  agg_val varchar;
BEGIN
  -- Drop the NULL placeholders array_agg keeps for rows that did not produce a
  -- semimod gate (provenance_semimod returns NULL for a NULL aggregated value),
  -- so a NULL input never participates in the aggregate.
  tokens := array_remove(tokens, NULL);
  c:=COALESCE(array_length(tokens, 1), 0);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    -- aggfnoid must be part of the UUID: SUM(id) and AVG(id) over the
    -- same children would otherwise collapse to a single gate, and
    -- their concurrent set_infos calls would overwrite each other's
    -- aggregation operator (resulting in the wrong agg_kind being
    -- read by provsql_having under cross-backend contention).  The
    -- scalar-aggregation flag must likewise be hashed: a scalar and a
    -- grouped aggregate over identical children carry different info2 and
    -- must stay distinct gates, else the concurrent set_infos calls would
    -- clobber the flag.  The flag is stored in the high bit of info2 (the
    -- low 31 bits keep the result-type OID); aggtype itself is passed clean
    -- so the agg_token->scalar cast still finds a valid type.
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',aggfnoid,tokens,CASE WHEN is_scalar THEN 'S' ELSE '' END));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid,
                      CASE WHEN is_scalar THEN aggtype | (-2147483648) ELSE aggtype END);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/**
 * @brief Create a semimodule scalar multiplication gate
 *
 * Pairs a scalar value with a provenance token, used internally by
 * the query rewriter for aggregate provenance.
 *
 * @param val the scalar value
 * @param token the provenance token to multiply
 */
CREATE OR REPLACE FUNCTION provenance_semimod(val anyelement, token UUID)
  RETURNS UUID AS
$$
DECLARE
  semimod_token uuid;
  value_token uuid;
BEGIN
  -- A NULL value means this row does not participate in the aggregate (SQL
  -- aggregates ignore NULL inputs; only count(*) counts rows unconditionally,
  -- and it passes a constant 1 here).  Produce no semimod gate so the row is
  -- skipped when provenance_aggregate builds the agg gate.
  IF val IS NULL THEN
    RETURN NULL;
  END IF;

  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('value',CAST(val AS VARCHAR)))
    INTO value_token;
  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('semimod',value_token,token))
    INTO semimod_token;

  --create value gates
  PERFORM create_gate(value_token,'value');
  PERFORM set_extra(value_token, CAST(val AS VARCHAR));

  --create semimod gate
  PERFORM create_gate(semimod_token,'semimod',ARRAY[token::uuid,value_token]);

  RETURN semimod_token;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @} */

/** @defgroup probability Probability and Shapley values
 *  Functions for computing probabilities, expected values, and
 *  game-theoretic contribution measures (Shapley/Banzhaf values)
 *  from provenance circuits.
 *  @{
 */

/**
 * @brief Compute the probability of a provenance token
 *
 * Compiles the provenance circuit to d-DNNF and evaluates the
 * probability. The compilation method can be selected explicitly.
 *
 * @param token provenance token to evaluate
 * @param method knowledge compilation method (NULL for default)
 * @param arguments additional arguments for the method
 */
CREATE OR REPLACE FUNCTION probability_evaluate(
  token UUID,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
  'provsql','probability_evaluate' LANGUAGE C STABLE;

/**
 * @brief Cheap certified probability interval of a DNF-shaped circuit.
 *
 * Returns @c [lower,upper] with @c lower <= probability_evaluate(token) <=
 * @c upper, computed without compiling the circuit (the Olteanu-Huang d-tree
 * leaf bound).  Errors when @p token is not a monotone DNF over input leaves.
 */
CREATE OR REPLACE FUNCTION probability_bounds(
  token UUID,
  OUT lower DOUBLE PRECISION,
  OUT upper DOUBLE PRECISION) AS
  'provsql','probability_bounds' LANGUAGE C STABLE;

/**
 * @brief Compute the expected value of a probabilistic scalar
 *
 * Computes E[input | prov] for either an @c agg_token (discrete
 * SUM/MIN/MAX aggregation over Boolean-input gate_agg circuits, with
 * @c prov as the Boolean conditioning event) or a @c random_variable
 * (continuous distribution, traversed by the analytical / MC
 * evaluator from @c Expectation.cpp).
 *
 * Implementation: thin wrapper over @c moment(input, 1, prov, method,
 * arguments).  Both branches converge on the same machinery; the
 * agg_token side computes E[X] as the @f$k=1@f$ instance of the
 * @f$n^k@f$-tuple enumeration in @c agg_raw_moment, the
 * random_variable side calls @c compute_expectation through
 * @c rv_moment.
 *
 * @param input aggregate expression or random variable to compute E[·] of
 * @param prov provenance condition (defaults to gate_one(), i.e., unconditional)
 * @param method knowledge compilation method (agg_token path only)
 * @param arguments additional arguments for the method (agg_token path only)
 */
CREATE OR REPLACE FUNCTION expected(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
  SELECT moment(input, 1, prov, method, arguments);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Internal: shared C entry point for variance / moment / central_moment.
 *
 * The @c expected() SQL function reaches the Expectation evaluator
 * through @c provenance_evaluate_compiled(..., 'expectation', ...).
 * The variance / raw-moment / central-moment SQL functions need an
 * extra @p k integer argument that does not fit that dispatcher's
 * signature, so they go through this dedicated entry point.  Returns
 * E[X^k] when @p central is FALSE, or E[(X - E[X])^k] when TRUE.
 */
CREATE OR REPLACE FUNCTION rv_moment(
  token uuid, k integer, central boolean,
  prov uuid DEFAULT gate_one())
  RETURNS double precision
  AS 'provsql','rv_moment' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Compute the raw moment E[X^k | prov] of an agg_token aggregate
 *
 * Sister of @c expected() for the agg_token side of the polymorphic
 * @c moment / @c variance / @c central_moment dispatch.  Supports the
 * same aggregation functions as @c expected: SUM (which COUNT
 * normalises to at the gate level via @c Aggregation.cpp:322), MIN,
 * and MAX.
 *
 * Strategy:
 * - <b>SUM</b>: with X = Σᵢ Iᵢ·vᵢ (Iᵢ the per-row inclusion indicator,
 *   vᵢ the row's value), expanding X^k and taking expectation gives
 *   @f$E[X^k] = \sum_{(i_1,\ldots,i_k) \in \{1..n\}^k} v_{i_1}\cdots v_{i_k}
 *               \cdot P(\bigwedge_{i \in \text{distinct}(i_1..i_k)} I_i)@f$.
 *   We enumerate the @f$n^k@f$ tuples, conjoin the distinct inclusion
 *   tokens (and @p prov when conditioning), and evaluate the
 *   probability via @c probability_evaluate.
 * - <b>MIN / MAX</b>: replace @c v with @c v^k in the rank-based
 *   enumeration that @c expected already uses; @c MAX is handled by
 *   sign-flipping per the existing trick (negate vs.  rerank), with
 *   the outer multiplier becoming @f$(-1)^k@f$ instead of just @f$-1@f$.
 *
 * Cost: SUM is @f$O(n^k)@f$ probability evaluations -- tractable for
 * small @p k or small @p n; for larger sizes, prefer reaching for the
 * sampler.  MIN / MAX stay linear in @p n.
 */
CREATE OR REPLACE FUNCTION agg_raw_moment(
  token agg_token,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  pair_children uuid[];
  n integer;
  i integer;
  j integer;
  vals float8[];
  toks uuid[];
  total float8;
  total_probability float8;
  tup integer[];
  d integer;
  prod_v float8;
  distinct_tok uuid[];
  conj_token uuid;
  prob float8;
  sign_max float8;
BEGIN
  IF token IS NULL OR k IS NULL THEN
    RETURN NULL;
  END IF;
  IF k < 0 THEN
    RAISE EXCEPTION 'agg_raw_moment(): k must be non-negative (got %)', k;
  END IF;
  IF get_gate_type(token) <> 'agg' THEN
    IF get_gate_type(token) IN ('arith', 'conditioned') THEN
      RAISE EXCEPTION 'expected / variance / moment over an arithmetic '
        'combination of aggregates (e.g. SUM(x) + SUM(y) or SUM(x) + 5), or a '
        'conditioning of one, is not yet supported: a moment can be taken only '
        'over a single aggregate (SUM / COUNT / MIN / MAX), optionally '
        'conditioned (SUM(x) | C)'
        USING HINT = 'Take the moment of each aggregate separately, or condition '
          'the bare aggregate.';
    ELSE
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation';
    END IF;
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
    -- count(col) keeps the COUNT identity at the gate level but its value is a
    -- SUM of per-row 0/1 indicators, so its moments are computed exactly like
    -- SUM (and its empty group is the real value 0, like SUM).  count(*)
    -- arrives here as 'sum' (it normalises to F_SUM_INT4); count(col) as 'count'.
    -- Trivial empty aggregation: SUM = 0, so SUM^k = 0 for k >= 1.
    -- Note: agg_token semantics treat the "no row included" world as
    -- SUM = 0, so this stays consistent with k = 1 (= expected()).
    IF n = 0 THEN
      RETURN 0;
    END IF;

    -- Extract per-child token + value arrays.
    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;

    -- Enumerate all k-tuples (i_1, ..., i_k) in {1..n}^k.  tup is the
    -- current tuple; we step through them in lexicographic order.
    total := 0;
    tup := array_fill(1, ARRAY[k]);
    LOOP
      prod_v := 1;
      FOR j IN 1..k LOOP
        prod_v := prod_v * vals[tup[j]];
      END LOOP;

      SELECT array_agg(DISTINCT toks[idx]) INTO distinct_tok
        FROM unnest(tup) AS idx;

      IF prov <> gate_one() THEN
        distinct_tok := distinct_tok || prov;
      END IF;
      conj_token := provenance_times(VARIADIC distinct_tok);
      prob := probability_evaluate(conj_token, method, arguments);

      total := total + prod_v * prob;

      d := k;
      WHILE d >= 1 AND tup[d] = n LOOP
        tup[d] := 1;
        d := d - 1;
      END LOOP;
      EXIT WHEN d = 0;
      tup[d] := tup[d] + 1;
    END LOOP;
  ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
    -- Rank enumeration: per distinct value v, P(MIN = v) is the
    -- probability that some t_i with v_i=v is true and all t_j with
    -- smaller v are false.  For MAX we negate values so the same
    -- "smaller-than" rank logic computes MIN-of-negated, then flip.
    -- The outer multiplier picks up the right sign for the k-th moment
    -- of MAX: E[MAX^k] = (-1)^k * E[MIN(-v)^k], so sign_max = (-1)^k.
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    -- MIN/MAX over the empty input world are NULL (no elements), not ±Infinity:
    -- SQL returns one row with a NULL value.  The moment is therefore CONDITIONAL
    -- on the aggregate being defined (non-empty) -- the empty world is excluded
    -- and the result renormalised by P(prov AND non-empty).  (count, whose empty
    -- value 0 is a real value, keeps the empty world; sum keeps it too, as 0.)
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: MIN/MAX undefined
    END IF;

    -- Numerator E[MIN^k . 1{prov AND non-empty}] (the rank sum naturally omits
    -- the empty world, since every term requires a present token).
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT sign_max * COALESCE(SUM(p * power(v, k)), 0) FROM (
        SELECT t1.v AS v,
          probability_evaluate(
            CASE WHEN prov = gate_one()
                 THEN provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                       provenance_plus(ARRAY_AGG(t2.tok)))
                 ELSE provenance_times(prov,
                        provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                         provenance_plus(ARRAY_AGG(t2.tok)))) END,
            method, arguments) AS p
        FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
        GROUP BY t1.v) tmp
      INTO total;

    -- Denominator P(prov AND non-empty) = P(prov (x) (+) tokens).
    SELECT probability_evaluate(
             CASE WHEN prov = gate_one()
                  THEN provenance_plus(ARRAY_AGG(tok))
                  ELSE provenance_times(prov, provenance_plus(ARRAY_AGG(tok))) END,
             method, arguments)
      FROM (SELECT (get_children(c))[1] AS tok FROM UNNEST(child_pairs) AS c) s
      INTO total_probability;

    IF total_probability <= epsilon() THEN
      RETURN NULL;  -- never defined under prov: MIN/MAX undefined
    END IF;
    RETURN total / total_probability;  -- already conditional; skip generic norm
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function;
  END IF;

  -- Conditional normalisation: E[X^k · 1_A] / P(A) = E[X^k | A].
  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Compute the variance Var[X | prov] of a probabilistic scalar
 *
 * Polymorphic dispatcher that mirrors @c expected: @c random_variable
 * inputs go through the analytical / MC evaluator
 * (@c rv_moment(uuid, 2, true)); @c agg_token inputs go through the
 * @c agg_raw_moment helper, computing
 * @f$\mathrm{Var}[X|A] = E[X^2|A] - E[X|A]^2@f$.  Conditioning on
 * @c prov is supported for @c agg_token (matching @c expected) but
 * not yet for @c random_variable.
 */
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

/**
 * @brief Compute the raw moment E[X^k | prov] of a probabilistic scalar
 *
 * @c k must be a non-negative integer.  @c k = 0 returns 1; @c k = 1
 * is equivalent to @c expected(input).  Polymorphic dispatcher: routes
 * @c random_variable through @c rv_moment (analytical / MC) and
 * @c agg_token through @c agg_raw_moment (SUM via tuple enumeration,
 * MIN / MAX via rank enumeration).
 */
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

/**
 * @brief Internal: rv-side support computation
 *
 * Lifts @c provsql::compute_support out of @c RangeCheck.cpp -- the
 * same interval-arithmetic propagation @c runRangeCheck uses to
 * decide @c gate_cmps.  Returns @c [-Infinity, +Infinity] when the
 * tightest bound is the conservative all-real interval (e.g. for a
 * normal RV, or any sub-circuit that mixes a normal in).
 */
CREATE OR REPLACE FUNCTION rv_support(
  token uuid, prov uuid DEFAULT gate_one(),
  OUT lo float8, OUT hi float8)
  AS 'provsql','rv_support' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Compute the support interval @c [lo, hi] of a probabilistic
 *        (or deterministic) scalar
 *
 * Polymorphic dispatcher mirroring @c expected / @c variance /
 * @c moment / @c central_moment, with two extra "free" branches:
 *
 * - <b>Plain numeric</b> (@c smallint / @c integer / @c bigint /
 *   @c numeric / @c real / @c double @c precision): degenerate
 *   point support @f$[c, c]@f$.  Lets callers ask for the support
 *   of a literal without round-tripping through @c as_random.
 * - <b>@c random_variable / bare @c uuid</b> (any provenance gate
 *   token; the @c random_variable branch reinterprets the value via
 *   the binary-coercible @c random_variable @c -> @c uuid cast):
 *   routes to @c rv_support, which propagates distribution
 *   supports (uniform exact, exponential @c [0,+∞), normal
 *   @c (-∞,+∞)) through @c gate_arith via interval arithmetic.
 *   @c gate_value gives the same @f$[c, c]@f$ point support as the
 *   numeric branch; any non-scalar gate (Boolean gates, aggregates,
 *   ...) safely falls back to the conservative all-real interval
 *   without raising.  Conditioning on @c prov is not yet supported.
 *
 * - @c agg_token: closed-form per aggregation function:
 *   - @c SUM : @f$[\sum_i \min(0,v_i), \sum_i \max(0,v_i)]@f$
 *     (every row is independently in or out of the included set; the
 *     extreme SUMs are reached by including only positive or only
 *     negative-valued rows).
 *   - @c MIN : @f$[\min_i v_i, \max_i v_i]@f$ in the non-empty
 *     subsets, plus @c +Infinity if the empty subset has positive
 *     probability under @c prov.
 *   - @c MAX : symmetric -- @c -Infinity if empty has positive
 *     probability under @c prov, otherwise @c min_i v_i; @c hi is
 *     always @c max_i v_i.
 *
 * Other aggregation functions raise.
 *
 * Returns the composite record @c (lo, hi) via the function's
 * @c OUT parameters, with @c -Infinity / @c +Infinity marking
 * unbounded ends.
 */
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

/**
 * @brief Compute the central moment E[(X - E[X|prov])^k | prov]
 *
 * @c k = 0 returns 1; @c k = 1 returns 0; @c k = 2 is equivalent to
 * @c variance(input, prov, ...).  Polymorphic dispatcher: routes
 * @c random_variable through @c rv_moment, and @c agg_token through
 * the binomial expansion
 * @f$E[(X-\mu)^k|A] = \sum_{i=0}^{k} \binom{k}{i} (-\mu)^{k-i} E[X^i|A]@f$
 * with @f$\mu = E[X|A]@f$, where each @f$E[X^i|A]@f$ comes from
 * @c agg_raw_moment.
 */
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

/**
 * @brief Compute the Shapley value of an input variable
 *
 * Measures the contribution of a specific input variable to the
 * truth of a provenance expression, using game-theoretic Shapley values.
 *
 * @param token provenance token to evaluate
 * @param variable UUID of the input variable
 * @param method knowledge compilation method
 * @param arguments additional arguments for the method
 * @param banzhaf if true, compute the Banzhaf value instead
 */
CREATE OR REPLACE FUNCTION shapley(
  token UUID,
  variable UUID,
  method text = NULL,
  arguments text = NULL,
  banzhaf BOOLEAN = 'f')
  RETURNS DOUBLE PRECISION AS
  'provsql','shapley' LANGUAGE C STABLE;

/** @brief Compute Shapley values for all input variables at once */
CREATE OR REPLACE FUNCTION shapley_all_vars(
  IN token UUID,
  IN method text = NULL,
  IN arguments text = NULL,
  IN banzhaf BOOLEAN = 'f',
  OUT variable UUID,
  OUT value DOUBLE PRECISION)
  RETURNS SETOF record AS
  'provsql', 'shapley_all_vars'
  LANGUAGE C STABLE;

/** @brief Compute the Banzhaf power index of an input variable */
CREATE OR REPLACE FUNCTION banzhaf(
  token UUID,
  variable UUID,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
  $$ SELECT provsql.shapley(token, variable, method, arguments, 't') $$
  LANGUAGE SQL;

/** @brief Compute Banzhaf power indices for all input variables at once */
CREATE OR REPLACE FUNCTION banzhaf_all_vars(
  IN token UUID,
  IN method text = NULL,
  IN arguments text = NULL,
  OUT variable UUID,
  OUT value DOUBLE PRECISION)
  RETURNS SETOF record AS
  $$ SELECT * FROM provsql.shapley_all_vars(token, method, arguments, 't') $$
  LANGUAGE SQL;

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
 * @param[out] probability the reachability probability
 * @param[out] data_treewidth treewidth of the min-fill decomposition of the
 *        data graph
 * @param[out] nb_bags number of bags in the decomposition
 * @param[out] max_states maximum number of dynamic-programming states at any
 *        decomposition node
 * @param[out] nb_gates number of gates in the emitted d-DNNF
 * @param[out] nb_variables number of variables in the emitted d-DNNF
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
 * @brief Boolean UCQ probability plus compilation statistics
 * (columnar form, internal)
 *
 * Same compilation as @c ucq_joint_compile_stats(query jsonb, ...),
 * returning the probability together with the three width columns that
 * substantiate thesis Prop. 4.2.11 empirically -- the adversarial family
 * has small data and circuit widths but large joint width -- and the
 * structural statistics.
 *
 * @param disjunct_nvars number of query variables of each disjunct
 * @param atom_disjunct disjunct index of each atom (parallel to @p atom_rel)
 * @param atom_rel relation id of each atom
 * @param atom_vars query-variable indices of all atom columns, concatenated
 * @param atom_arity number of columns of each atom (slices @p atom_vars)
 * @param fact_rel relation id of each fact
 * @param fact_elems element ids of all fact columns, concatenated
 * @param fact_arity number of columns of each fact (slices @p fact_elems)
 * @param fact_tokens provenance token of each fact
 * @param fact_probs probability of each fact
 * @param[out] probability the exact UCQ probability
 * @param[out] joint_treewidth width of the min-fill decomposition found
 * @param[out] data_treewidth_lb degeneracy lower bound of the data-only graph
 * @param[out] circuit_treewidth_lb degeneracy lower bound of the slice-only graph
 * @param[out] n_bags number of bags in the decomposition
 * @param[out] max_states peak number of DP states at any node
 * @param[out] dd_size number of gates in the emitted d-D
 * @param[out] n_enumerating maximum number of essential (enumerating) query
 *        variables over the disjuncts -- the @c e of the @f$2^{O(k^e)}@f$
 *        bound, with variables functionally determined by others (via FDs
 *        mined from the data) removed
 */
CREATE OR REPLACE FUNCTION ucq_joint_compile_stats(
  IN disjunct_nvars INT[],
  IN atom_disjunct INT[],
  IN atom_rel INT[],
  IN atom_vars INT[],
  IN atom_arity INT[],
  IN fact_rel INT[],
  IN fact_elems INT[],
  IN fact_arity INT[],
  IN fact_tokens UUID[],
  IN fact_probs DOUBLE PRECISION[],
  OUT probability DOUBLE PRECISION,
  OUT joint_treewidth INT,
  OUT data_treewidth_lb INT,
  OUT circuit_treewidth_lb INT,
  OUT n_bags BIGINT,
  OUT max_states BIGINT,
  OUT dd_size BIGINT,
  OUT n_enumerating INT)
  AS 'provsql','ucq_joint_compile_stats'
  LANGUAGE C IMMUTABLE PARALLEL SAFE;


/**
 * @brief Boolean UCQ probability plus statistics from a JSON specification
 *
 * JSON-spec wrapper over the columnar @c ucq_joint_compile_stats()
 * (see @c ucq_joint_evaluate(query jsonb, ...) for the JSON format).
 */
CREATE OR REPLACE FUNCTION ucq_joint_compile_stats(
  IN query JSONB,
  IN fact_rel INT[],
  IN fact_elems INT[],
  IN fact_arity INT[],
  IN fact_tokens UUID[],
  IN fact_probs DOUBLE PRECISION[],
  OUT probability DOUBLE PRECISION,
  OUT joint_treewidth INT,
  OUT data_treewidth_lb INT,
  OUT circuit_treewidth_lb INT,
  OUT n_bags BIGINT,
  OUT max_states BIGINT,
  OUT dd_size BIGINT,
  OUT n_enumerating INT)
  AS $$
DECLARE
  dnv INT[] := '{}'; adisj INT[] := '{}'; arel INT[] := '{}';
  avars INT[] := '{}'; aarity INT[] := '{}';
  d JSONB; a JSONB; v TEXT; didx INT := 0;
BEGIN
  FOR d IN SELECT * FROM jsonb_array_elements(query->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx;
      arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;
  SELECT s.probability, s.joint_treewidth, s.data_treewidth_lb,
         s.circuit_treewidth_lb, s.n_bags, s.max_states, s.dd_size,
         s.n_enumerating
    INTO probability, joint_treewidth, data_treewidth_lb,
         circuit_treewidth_lb, n_bags, max_states, dd_size, n_enumerating
    FROM ucq_joint_compile_stats(dnv, adisj, arel, avars, aarity,
      fact_rel, fact_elems, fact_arity, fact_tokens, fact_probs) s;
END;
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;











/**
 * @brief Correlated Boolean UCQ probability plus compilation statistics
 * (columnar form, internal)
 *
 * Same compilation as @c ucq_joint_evaluate_tracked(); the three width
 * columns substantiate thesis Prop. 4.2.11 on real correlated data (the
 * data-only and circuit-only degeneracy bounds can be small while the
 * joint width is large).
 */
CREATE OR REPLACE FUNCTION ucq_joint_compile_stats_tracked(
  IN disjunct_nvars INT[],
  IN atom_disjunct INT[],
  IN atom_rel INT[],
  IN atom_vars INT[],
  IN atom_arity INT[],
  IN fact_rel INT[],
  IN fact_elems INT[],
  IN fact_arity INT[],
  IN fact_tokens UUID[],
  OUT probability DOUBLE PRECISION,
  OUT joint_treewidth INT,
  OUT data_treewidth_lb INT,
  OUT circuit_treewidth_lb INT,
  OUT n_bags BIGINT,
  OUT max_states BIGINT,
  OUT dd_size BIGINT,
  OUT n_enumerating INT)
  AS 'provsql','ucq_joint_compile_stats_tracked'
  LANGUAGE C STABLE PARALLEL SAFE;


/**
 * @brief Correlated Boolean UCQ probability plus statistics from a JSON spec
 */
CREATE OR REPLACE FUNCTION ucq_joint_compile_stats_tracked(
  IN query JSONB,
  IN fact_rel INT[],
  IN fact_elems INT[],
  IN fact_arity INT[],
  IN fact_tokens UUID[],
  OUT probability DOUBLE PRECISION,
  OUT joint_treewidth INT,
  OUT data_treewidth_lb INT,
  OUT circuit_treewidth_lb INT,
  OUT n_bags BIGINT,
  OUT max_states BIGINT,
  OUT dd_size BIGINT,
  OUT n_enumerating INT)
  AS $$
DECLARE
  dnv INT[] := '{}'; adisj INT[] := '{}'; arel INT[] := '{}';
  avars INT[] := '{}'; aarity INT[] := '{}';
  d JSONB; a JSONB; v TEXT; didx INT := 0;
BEGIN
  FOR d IN SELECT * FROM jsonb_array_elements(query->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx;
      arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;
  SELECT s.probability, s.joint_treewidth, s.data_treewidth_lb,
         s.circuit_treewidth_lb, s.n_bags, s.max_states, s.dd_size,
         s.n_enumerating
    INTO probability, joint_treewidth, data_treewidth_lb,
         circuit_treewidth_lb, n_bags, max_states, dd_size, n_enumerating
    FROM ucq_joint_compile_stats_tracked(dnv, adisj, arel, avars, aarity,
      fact_rel, fact_elems, fact_arity, fact_tokens) s;
END;
$$ LANGUAGE plpgsql STABLE PARALLEL SAFE;

/**
 * @brief Compile a correlated UCQ and materialise its certified d-D,
 * returning the root provenance token (columnar form, internal)
 *
 * The architecturally-primary route: the compiler builds the
 * deterministic, decomposable circuit and materialises it as ordinary
 * @c plus / @c times / @c monus provenance gates (carrying the d-DNNF
 * certificate); the answer is then obtained through the standard entry
 * points on the returned token -- @c probability_evaluate(token),
 * @c shapley(token, ...), expectation -- so the joint-width path shares
 * the one evaluation pipeline.  The token is the exact Boolean
 * provenance of the UCQ (no @c 'absorptive' marker).
 */
CREATE OR REPLACE FUNCTION ucq_joint_materialize_tracked(
  disjunct_nvars INT[],
  atom_disjunct INT[],
  atom_rel INT[],
  atom_vars INT[],
  atom_arity INT[],
  fact_rel INT[],
  fact_elems INT[],
  fact_arity INT[],
  fact_tokens UUID[])
  RETURNS UUID AS
  'provsql','ucq_joint_materialize_tracked' LANGUAGE C VOLATILE;

/**
 * @brief Compile a correlated UCQ and materialise its certified d-D
 * from a JSON spec, returning the root provenance token
 *
 * JSON-spec wrapper over @c ucq_joint_materialize_tracked().  Evaluate
 * the answer with the standard surface, e.g.
 * @c probability_evaluate(ucq_joint_materialize_tracked(query, ...)).
 */
CREATE OR REPLACE FUNCTION ucq_joint_materialize_tracked(
  query JSONB,
  fact_rel INT[],
  fact_elems INT[],
  fact_arity INT[],
  fact_tokens UUID[])
  RETURNS UUID AS $$
DECLARE
  dnv INT[] := '{}'; adisj INT[] := '{}'; arel INT[] := '{}';
  avars INT[] := '{}'; aarity INT[] := '{}';
  d JSONB; a JSONB; v TEXT; didx INT := 0;
BEGIN
  FOR d IN SELECT * FROM jsonb_array_elements(query->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx;
      arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;
  RETURN ucq_joint_materialize_tracked(dnv, adisj, arel, avars, aarity,
    fact_rel, fact_elems, fact_arity, fact_tokens);
END;
$$ LANGUAGE plpgsql VOLATILE;

/**
 * @brief Compile a UCQ over named relations into a materialised certified
 * d-D, gathering the facts from the store -- the descriptor-driven engine
 *
 * The query-surface bridge for the joint-width compiler: instead of
 * hand-built columnar arrays, a JSON @p descriptor names the relations
 * and how their columns map to query variables, and this function
 * gathers the facts itself (the provenance rewriting is disabled around
 * the gather), builds the value-based element dictionary shared across
 * the relations (so equal join values get the same dense id), compiles
 * and materialises the certified d-D, and returns its provenance token.
 * The answer is then any standard evaluation on that token --
 * @c probability_evaluate(ucq_joint_provenance(...)),
 * @c shapley(...), expectation.  This is also the engine the planner-time
 * query recogniser drives once it builds the descriptor from a query's
 * abstract syntax.
 *
 * Descriptor shape:
 * @verbatim
 * { "disjuncts": [ { "n_vars": k,
 *                    "atoms": [ {"rel": <relidx>, "vars": [..]}, ... ] }, ... ],
 *   "relations": [ "schema.r", "schema.s", ... ],   -- relidx -> relation
 *   "elem_cols": [ ["x"], ["x","y"], ... ] }         -- per relation: the
 *                                                       element columns, in
 *                                                       the atom's var order
 * @endverbatim
 *
 * @param descriptor the UCQ + the relations and their element columns
 * @param fallback token returned if the joint-width compiler declines
 * @return the materialised joint-width provenance token (NULL UUID-free
 *         exact Boolean provenance of the UCQ)
 */
CREATE OR REPLACE FUNCTION ucq_joint_provenance(
  descriptor JSONB, fallback UUID DEFAULT NULL)
RETURNS UUID AS $$
DECLARE
  legs text; sql text; saved text;
  fact_rel int[]; fact_elems int[]; fact_arity int[]; fact_tokens uuid[];
  dnv int[]:='{}'; adisj int[]:='{}'; arel int[]:='{}';
  avars int[]:='{}'; aarity int[]:='{}';
  d jsonb; a jsonb; v text; didx int:=0;
BEGIN
  -- Parse the UCQ structure into the columnar query arrays.
  FOR d IN SELECT * FROM jsonb_array_elements(descriptor->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx; arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;

  -- One UNION ALL leg per relation: (relation index, text element array,
  -- provenance token).  No temp tables: a single gather query, with the
  -- value-based dense element dictionary built inline.
  SELECT string_agg(
           format('SELECT %s, ARRAY[%s]::text[], provsql FROM %s%s',
             rn - 1,
             (SELECT string_agg(format('(%I)::text', c), ',')
                FROM jsonb_array_elements_text(descriptor->'elem_cols'->(rn-1)::int) c),
             rel,
             -- the lifted single-relation selection (a pre-filter), already
             -- deparsed to SQL by the recogniser; '' / absent = unfiltered.
             CASE WHEN coalesce(descriptor->'rel_where'->>(rn-1)::int,'') <> ''
                  THEN ' WHERE '||(descriptor->'rel_where'->>(rn-1)::int)
                  ELSE '' END),
           ' UNION ALL ')
    INTO legs
    FROM jsonb_array_elements_text(descriptor->'relations') WITH ORDINALITY t(rel, rn);

  sql := format($q$
    WITH facts(rel,elems,tok) AS (%s),
         ord AS (SELECT row_number() OVER () AS ord, rel, elems, tok FROM facts),
         dict AS (SELECT val, (dense_rank() OVER (ORDER BY val))-1 AS id
                    FROM (SELECT DISTINCT unnest(elems) AS val FROM facts) u)
    SELECT (SELECT array_agg(rel ORDER BY ord) FROM ord),
           (SELECT array_agg(cardinality(elems) ORDER BY ord) FROM ord),
           (SELECT array_agg(tok ORDER BY ord) FROM ord),
           (SELECT array_agg(dd.id ORDER BY o.ord, e.k)
              FROM ord o, LATERAL unnest(o.elems) WITH ORDINALITY e(val,k)
              JOIN dict dd ON dd.val = e.val)
  $q$, legs);

  -- Read the raw rows with provenance rewriting disabled (we only read
  -- the existing provsql column; this internal gather is not tracked).
  saved := current_setting('provsql.active', true);
  PERFORM set_config('provsql.active','off', true);
  EXECUTE sql INTO fact_rel, fact_arity, fact_tokens, fact_elems;
  PERFORM set_config('provsql.active', saved, true);

  RETURN ucq_joint_materialize_tracked(dnv,adisj,arel,avars,aarity,
    fact_rel,fact_elems,fact_arity,fact_tokens);
EXCEPTION WHEN OTHERS THEN
  -- The joint-width compiler declined (unsupported gate type, joint
  -- width too large, ...): fall back to the normal provenance so the
  -- query never fails.  Both give the same probability.
  RETURN fallback;
END;
$$ LANGUAGE plpgsql VOLATILE;

-- ===========================================================================
-- Safe-UCQ Möbius-inversion route (mobius_evaluate.cpp).
--
-- The last missing exact route of the Dalvi-Suciu dichotomy: UCQs that are
-- safe only because the \#P-hard terms of their inclusion-exclusion expansion
-- carry a zero Möbius value on the CNF lattice and cancel (canonical witness:
-- QW / q9).  Same TID gather as ucq_joint, then the lattice-walking compiler
-- materialises a gate_mobius-rooted circuit (a signed combination over
-- certified-independent islands), answered in PTIME data complexity by the
-- standard probability path.
-- ===========================================================================

/**
 * @brief Materialise the safe-UCQ Möbius circuit and return its root token.
 *        Columnar (TID) interface; see ucq_mobius_provenance for the gather.
 */
CREATE OR REPLACE FUNCTION ucq_mobius_materialize_tracked(
  disjunct_nvars INT[],
  atom_disjunct INT[],
  atom_rel INT[],
  atom_vars INT[],
  atom_arity INT[],
  fact_rel INT[],
  fact_elems INT[],
  fact_arity INT[],
  fact_tokens UUID[],
  lineage UUID DEFAULT NULL)
  RETURNS UUID AS
  'provsql','ucq_mobius_materialize_tracked' LANGUAGE C VOLATILE;

/**
 * @brief Compile the Möbius circuit and return the lattice statistics plus the
 *        probability (the demonstrability surface).  @c cancelled_hard is the
 *        single number that makes the mechanism legible: for q9 the 1 cancelled
 *        element is \#P-hard, so the query is easy only because its hard part
 *        cancels.
 */
CREATE OR REPLACE FUNCTION ucq_mobius_compile_stats(
  IN disjunct_nvars INT[],
  IN atom_disjunct INT[],
  IN atom_rel INT[],
  IN atom_vars INT[],
  IN atom_arity INT[],
  IN fact_rel INT[],
  IN fact_elems INT[],
  IN fact_arity INT[],
  IN fact_tokens UUID[],
  OUT probability DOUBLE PRECISION,
  OUT n_components INT,
  OUT n_cnf_conjuncts INT,
  OUT lattice_size INT,
  OUT n_nonzero INT,
  OUT n_cancelled INT,
  OUT cancelled_hard BOOLEAN,
  OUT dd_size BIGINT,
  OUT memo_hits BIGINT)
  AS 'provsql','ucq_mobius_compile_stats'
  LANGUAGE C VOLATILE;

/**
 * @brief Pass a token through iff it is a @c gate_mobius, else return NULL.
 *
 * The Möbius-precedence dispatch (see @c make_provenance_expression) wraps the
 * Möbius call in this and then @c COALESCE\ s it before the joint-width call:
 * a Möbius *success* always roots a @c gate_mobius (the compiler wraps even a
 * thin selector around the lineage), so it short-circuits and the joint-width
 * compiler never runs; a Möbius *decline* returns the literal lineage (never a
 * @c gate_mobius), so this yields NULL and @c COALESCE falls through to
 * joint-width.  The lineage token is a plain plus/times/input, so the test is
 * unambiguous.
 */
CREATE OR REPLACE FUNCTION mobius_or_null(tok UUID)
RETURNS UUID AS $$
  SELECT CASE WHEN tok IS NOT NULL AND provsql.get_gate_type(tok) = 'mobius'
              THEN tok END
$$ LANGUAGE sql STABLE;

/**
 * @brief Möbius-route provenance from a descriptor (the planner-substituted
 *        entry point, and the manual one).  Same descriptor and TID gather as
 *        @c ucq_joint_provenance; on any decline (unsafe shape, cap, not TID)
 *        returns @p fallback, so a recognised query never fails.
 */
CREATE OR REPLACE FUNCTION ucq_mobius_provenance(
  descriptor JSONB, fallback UUID DEFAULT NULL)
RETURNS UUID AS $$
DECLARE
  legs text; sql text; saved text;
  fact_rel int[]; fact_elems int[]; fact_arity int[]; fact_tokens uuid[];
  dnv int[]:='{}'; adisj int[]:='{}'; arel int[]:='{}';
  avars int[]:='{}'; aarity int[]:='{}';
  d jsonb; a jsonb; v text; didx int:=0;
BEGIN
  FOR d IN SELECT * FROM jsonb_array_elements(descriptor->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx; arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;

  SELECT string_agg(
           format('SELECT %s, ARRAY[%s]::text[], provsql FROM %s%s',
             rn - 1,
             (SELECT string_agg(format('(%I)::text', c), ',')
                FROM jsonb_array_elements_text(descriptor->'elem_cols'->(rn-1)::int) c),
             rel,
             CASE WHEN coalesce(descriptor->'rel_where'->>(rn-1)::int,'') <> ''
                  THEN ' WHERE '||(descriptor->'rel_where'->>(rn-1)::int)
                  ELSE '' END),
           ' UNION ALL ')
    INTO legs
    FROM jsonb_array_elements_text(descriptor->'relations') WITH ORDINALITY t(rel, rn);

  sql := format($q$
    WITH facts(rel,elems,tok) AS (%s),
         ord AS (SELECT row_number() OVER () AS ord, rel, elems, tok FROM facts),
         dict AS (SELECT val, (dense_rank() OVER (ORDER BY val))-1 AS id
                    FROM (SELECT DISTINCT unnest(elems) AS val FROM facts) u)
    SELECT (SELECT array_agg(rel ORDER BY ord) FROM ord),
           (SELECT array_agg(cardinality(elems) ORDER BY ord) FROM ord),
           (SELECT array_agg(tok ORDER BY ord) FROM ord),
           (SELECT array_agg(dd.id ORDER BY o.ord, e.k)
              FROM ord o, LATERAL unnest(o.elems) WITH ORDINALITY e(val,k)
              JOIN dict dd ON dd.val = e.val)
  $q$, legs);

  saved := current_setting('provsql.active', true);
  PERFORM set_config('provsql.active','off', true);
  EXECUTE sql INTO fact_rel, fact_arity, fact_tokens, fact_elems;
  PERFORM set_config('provsql.active', saved, true);

  -- Pass the normal-provenance fallback as the lineage: it is carried on the
  -- gate_mobius so the token still answers Shapley / semiring / PROV on the
  -- literal lineage (the Möbius combination is a probability-only shortcut).
  RETURN ucq_mobius_materialize_tracked(dnv,adisj,arel,avars,aarity,
    fact_rel,fact_elems,fact_arity,fact_tokens, fallback);
EXCEPTION WHEN OTHERS THEN
  RETURN fallback;
END;
$$ LANGUAGE plpgsql VOLATILE;

/**
 * @brief Möbius lattice statistics + probability from a descriptor: the
 *        demonstrability SRF (mirrors mobius_compile_stats in doc/TODO).  Gathers
 *        the same TID facts as @c ucq_mobius_provenance, then runs the columnar
 *        @c ucq_mobius_compile_stats.
 */
CREATE OR REPLACE FUNCTION mobius_compile_stats(
  IN descriptor JSONB,
  OUT probability DOUBLE PRECISION,
  OUT n_components INT,
  OUT n_cnf_conjuncts INT,
  OUT lattice_size INT,
  OUT n_nonzero INT,
  OUT n_cancelled INT,
  OUT cancelled_hard BOOLEAN,
  OUT dd_size BIGINT,
  OUT memo_hits BIGINT)
RETURNS RECORD AS $$
DECLARE
  legs text; sql text; saved text;
  fact_rel int[]; fact_elems int[]; fact_arity int[]; fact_tokens uuid[];
  dnv int[]:='{}'; adisj int[]:='{}'; arel int[]:='{}';
  avars int[]:='{}'; aarity int[]:='{}';
  d jsonb; a jsonb; v text; didx int:=0;
BEGIN
  FOR d IN SELECT * FROM jsonb_array_elements(descriptor->'disjuncts') LOOP
    dnv := dnv || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      adisj := adisj || didx; arel := arel || (a->>'rel')::int;
      aarity := aarity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        avars := avars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;

  SELECT string_agg(
           format('SELECT %s, ARRAY[%s]::text[], provsql FROM %s%s',
             rn - 1,
             (SELECT string_agg(format('(%I)::text', c), ',')
                FROM jsonb_array_elements_text(descriptor->'elem_cols'->(rn-1)::int) c),
             rel,
             CASE WHEN coalesce(descriptor->'rel_where'->>(rn-1)::int,'') <> ''
                  THEN ' WHERE '||(descriptor->'rel_where'->>(rn-1)::int)
                  ELSE '' END),
           ' UNION ALL ')
    INTO legs
    FROM jsonb_array_elements_text(descriptor->'relations') WITH ORDINALITY t(rel, rn);

  sql := format($q$
    WITH facts(rel,elems,tok) AS (%s),
         ord AS (SELECT row_number() OVER () AS ord, rel, elems, tok FROM facts),
         dict AS (SELECT val, (dense_rank() OVER (ORDER BY val))-1 AS id
                    FROM (SELECT DISTINCT unnest(elems) AS val FROM facts) u)
    SELECT (SELECT array_agg(rel ORDER BY ord) FROM ord),
           (SELECT array_agg(cardinality(elems) ORDER BY ord) FROM ord),
           (SELECT array_agg(tok ORDER BY ord) FROM ord),
           (SELECT array_agg(dd.id ORDER BY o.ord, e.k)
              FROM ord o, LATERAL unnest(o.elems) WITH ORDINALITY e(val,k)
              JOIN dict dd ON dd.val = e.val)
  $q$, legs);

  saved := current_setting('provsql.active', true);
  PERFORM set_config('provsql.active','off', true);
  EXECUTE sql INTO fact_rel, fact_arity, fact_tokens, fact_elems;
  PERFORM set_config('provsql.active', saved, true);

  SELECT s.probability, s.n_components, s.n_cnf_conjuncts, s.lattice_size,
         s.n_nonzero, s.n_cancelled, s.cancelled_hard, s.dd_size, s.memo_hits
    INTO probability, n_components, n_cnf_conjuncts, lattice_size,
         n_nonzero, n_cancelled, cancelled_hard, dd_size, memo_hits
    FROM ucq_mobius_compile_stats(dnv,adisj,arel,avars,aarity,
      fact_rel,fact_elems,fact_arity,fact_tokens) s;
END;
$$ LANGUAGE plpgsql VOLATILE;

/**
 * @brief Internal gather for the per-answer joint route: parse @p descriptor
 *        into the columnar UCQ arrays and gather every fact (relation index,
 *        dense element ids, provenance token) with the value dictionary.
 *
 * Used only by the planner-substituted @c ucq_joint_provenance_answer (the C
 * single-DP entry point), which calls it ONCE per query and then computes all
 * answers in one sweep.  No head pinning: the single DP discovers the answers.
 * @c val_by_id maps a dense element id back to its text value (so an answer's
 * head ids can be matched to the @c GROUP @c BY head text).
 */
CREATE OR REPLACE FUNCTION ucq_joint_gather(
  descriptor JSONB,
  OUT disjunct_nvars INT[], OUT atom_disjunct INT[], OUT atom_rel INT[],
  OUT atom_vars INT[], OUT atom_arity INT[],
  OUT fact_rel INT[], OUT fact_elems INT[], OUT fact_arity INT[],
  OUT fact_tokens UUID[], OUT val_by_id TEXT[])
AS $$
DECLARE
  legs text; sql text; saved text; d jsonb; a jsonb; v text; didx int := 0;
BEGIN
  disjunct_nvars:='{}'; atom_disjunct:='{}'; atom_rel:='{}';
  atom_vars:='{}'; atom_arity:='{}';
  FOR d IN SELECT * FROM jsonb_array_elements(descriptor->'disjuncts') LOOP
    disjunct_nvars := disjunct_nvars || (d->>'n_vars')::int;
    FOR a IN SELECT * FROM jsonb_array_elements(d->'atoms') LOOP
      atom_disjunct := atom_disjunct || didx;
      atom_rel := atom_rel || (a->>'rel')::int;
      atom_arity := atom_arity || jsonb_array_length(a->'vars');
      FOR v IN SELECT * FROM jsonb_array_elements_text(a->'vars') LOOP
        atom_vars := atom_vars || v::int;
      END LOOP;
    END LOOP;
    didx := didx + 1;
  END LOOP;

  SELECT string_agg(
           format('SELECT %s, ARRAY[%s]::text[], provsql FROM %s%s', rn - 1,
             (SELECT string_agg(format('(%I)::text', c), ',')
                FROM jsonb_array_elements_text(descriptor->'elem_cols'->(rn-1)::int) c),
             rel,
             CASE WHEN coalesce(descriptor->'rel_where'->>(rn-1)::int,'') <> ''
                  THEN ' WHERE '||(descriptor->'rel_where'->>(rn-1)::int)
                  ELSE '' END),
           ' UNION ALL ')
    INTO legs
    FROM jsonb_array_elements_text(descriptor->'relations') WITH ORDINALITY t(rel, rn);

  sql := format($q$
    WITH facts(rel,elems,tok) AS (%s),
         ord AS (SELECT row_number() OVER () AS ord, rel, elems, tok FROM facts),
         dict AS (SELECT val, (dense_rank() OVER (ORDER BY val))-1 AS id
                    FROM (SELECT DISTINCT unnest(elems) AS val FROM facts) u)
    SELECT (SELECT array_agg(rel ORDER BY ord) FROM ord),
           (SELECT array_agg(cardinality(elems) ORDER BY ord) FROM ord),
           (SELECT array_agg(tok ORDER BY ord) FROM ord),
           (SELECT array_agg(dd.id ORDER BY o.ord, e.k)
              FROM ord o, LATERAL unnest(o.elems) WITH ORDINALITY e(val,k)
              JOIN dict dd ON dd.val = e.val),
           (SELECT array_agg(val ORDER BY id) FROM dict)
  $q$, legs);

  saved := current_setting('provsql.active', true);
  PERFORM set_config('provsql.active','off', true);
  EXECUTE sql INTO fact_rel, fact_arity, fact_tokens, fact_elems, val_by_id;
  PERFORM set_config('provsql.active', saved, true);
END;
$$ LANGUAGE plpgsql VOLATILE;

/**
 * @brief Per-answer joint-width provenance via the TOP-DOWN single DP
 *        (planner-substituted, C).
 *
 * The transparent per-answer rewrite substitutes one call per output group.
 * On the FIRST call of a query the function gathers the facts once
 * (@c ucq_joint_gather), runs the single DP, and materialises EVERY answer's
 * certified d-D into the store, caching @c head_vals -> token in @c fn_extra;
 * each subsequent group call is an O(1) lookup -- so the whole GROUP BY costs
 * one gather + one decomposition + one sweep, not @p k of each.  On any
 * decline (joint width too large) the @p fallback token (the normal
 * per-answer provenance) is returned, so the query never fails.  The answer's
 * marginal / Shapley / expectation is then the standard evaluation on the
 * returned token -- one pipeline for the whole system.
 */
CREATE OR REPLACE FUNCTION ucq_joint_provenance_answer(
  descriptor JSONB, head_vars INT[], head_vals TEXT[], fallback UUID DEFAULT NULL)
RETURNS UUID AS 'provsql','ucq_joint_provenance_answer'
LANGUAGE C STABLE;

/**
 * @brief Per-answer safe-UCQ Möbius provenance (planner-substituted): one
 *        head-pinned Möbius circuit per output group.  On the first call the
 *        facts are gathered once (ucq_joint_gather) and cached; each group pins
 *        @p head_vars to @p head_vals and compiles, caching head -> token.  On
 *        any decline returns @p fallback.  STABLE: it caches per fn-call
 *        context, so it is not re-evaluated within one scan.
 */
CREATE OR REPLACE FUNCTION ucq_mobius_provenance_answer(
  descriptor JSONB, head_vars INT[], head_vals TEXT[], fallback UUID DEFAULT NULL)
RETURNS UUID AS 'provsql','ucq_mobius_provenance_answer'
LANGUAGE C STABLE;


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
 * @param[out] vertex a vertex reachable from some source
 * @param[out] token the materialised reachability provenance token of @c vertex
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
 * @param[out] vertex a reachable vertex
 * @param[out] hops the walk length at which @c vertex is reached
 * @param[out] token the materialised provenance token of the @c (vertex, hops) pair
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
 * @param[out] group_id a group whose every member is reachable
 * @param[out] token the materialised all-members-reachable provenance token of
 *        @c group_id
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
 *        relation's columns (table-qualified as @c t.column), restricting
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
      -- (deparsed table-qualified as t.column); the working table side
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
 *
 * @param[out] sources source vertex (dense ID) of each gathered edge
 * @param[out] destinations destination vertex (dense ID) of each edge
 * @param[out] tokens provenance token of each edge tuple
 * @param[out] probabilities probability of each edge tuple
 * @param[out] block_keys per-edge BID key variable (nil UUID = independent)
 * @param[out] block_indices per-edge outcome index within its block
 * @param[out] extra_ids dense IDs assigned to the @p extra_vertices
 * @param[out] vertices dense-ID-to-original-value map (text, 1-indexed)
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
 * @param[out] source_values vertex value of each source tuple (as text)
 * @param[out] source_tokens per-source base @c input token (nil UUID = certain)
 * @param[out] source_probabilities per-source probability
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



/** @} */

/** @defgroup provenance_output Provenance output
 *  Functions for visualizing and exporting provenance circuits
 *  in various formats.
 *  @{
 */

/**
 * @brief Return a DOT or text visualization of the provenance circuit
 *
 * @param token root provenance token
 * @param token2desc mapping table for gate descriptions
 * @param dbg debug level (0 = normal)
 */
CREATE OR REPLACE FUNCTION view_circuit(
  token UUID,
  token2desc regclass,
  dbg int = 0)
  RETURNS TEXT AS
  'provsql','view_circuit' LANGUAGE C;

/**
 * @brief Return a DOT visualisation of the d-DNNF compiled from the
 * provenance circuit
 *
 * Runs the requested external knowledge compiler and renders the
 * resulting d-DNNF as a GraphViz digraph.
 *
 * @param token root provenance token
 * @param compiler external compiler or in-process meta-route to invoke;
 *   empty (the default) picks the highest-preference available compiler
 */
CREATE OR REPLACE FUNCTION compile_to_ddnnf_dot(
  token UUID,
  compiler TEXT = '')
  RETURNS TEXT AS
  'provsql','compile_to_ddnnf_dot' LANGUAGE C;

/**
 * @brief Return the compiled d-DNNF of a provenance circuit in the
 * c2d / d4 ".nnf" text interchange format.
 *
 * Companion to compile_to_ddnnf_dot (DOT, for viewing): this is the
 * machine-readable form, suitable for feeding to an external d-DNNF
 * reasoner / verifier or saving next to tseytin_cnf (same variable
 * numbering). Accepts the same compiler / meta-route names.
 *
 * @param token root provenance token
 * @param compiler compiler or in-process meta-route to use; empty (the
 *   default) picks the highest-preference available compiler
 */
CREATE OR REPLACE FUNCTION compile_to_ddnnf(
  token UUID,
  compiler TEXT = '')
  RETURNS TEXT AS
  'provsql','compile_to_ddnnf' LANGUAGE C;

/**
 * @brief Structural statistics of the d-DNNF a compiler produces for a
 * provenance circuit.
 *
 * Compiles the circuit with the given compiler / meta-route (same names
 * as compile_to_ddnnf_dot: d4, d4v2, c2d, minic2d, dsharp, panini-*,
 * tree-decomposition, interpret-as-dd, default) and returns a jsonb
 * object: nodes, edges, and / or / not / inputs counts, smooth, depth
 * (longest path), treewidth (null when not computable), and compile_ms.
 * Lets clients compare what each compiler produces on the same circuit.
 *
 * @param token root provenance token
 * @param compiler compiler or in-process meta-route to use; empty (the
 *   default) picks the highest-preference available compiler
 */
CREATE OR REPLACE FUNCTION ddnnf_stats(
  token UUID,
  compiler TEXT = '')
  RETURNS jsonb AS
  'provsql','ddnnf_stats' LANGUAGE C;

/**
 * @brief Return the DIMACS CNF (Tseytin transformation) of the provenance circuit
 *
 * Returns the same encoding the extension writes to a temp file before
 * invoking d4 / c2d / minic2d / dsharp. With @c weighted true (the
 * default), per-input probability weights are appended as @c w lines.
 *
 * @param token root provenance token
 * @param weighted include probability weights when true
 * @param mapping prepend "c input <var> <uuid> <prob>" comment lines
 *        documenting which provenance input each variable stands for
 */
CREATE OR REPLACE FUNCTION tseytin_cnf(
  token UUID,
  weighted BOOLEAN = TRUE,
  mapping BOOLEAN = TRUE)
  RETURNS TEXT AS
  'provsql','tseytin_cnf' LANGUAGE C;

/**
 * @brief Map each DIMACS variable of tseytin_cnf back to its
 *        provenance input.
 *
 * Returns one row per input gate: the variable index (matching
 * tseytin_cnf and compile_to_ddnnf's NNF), the original-circuit UUID
 * of that input, and its probability. Lets a satisfying assignment or
 * weighted model count obtained from an external tool be read against
 * the provenance circuit.
 *
 * @param token root provenance token
 */
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

/**
 * @brief Return a DOT visualisation of the tree decomposition of the
 * provenance circuit
 *
 * Computes the min-fill decomposition used by the in-process
 * knowledge compiler. The first line of the output is a comment of
 * the form @c "// treewidth=<n>".
 *
 * @param token root provenance token
 */
CREATE OR REPLACE FUNCTION tree_decomposition_dot(
  token UUID)
  RETURNS TEXT AS
  'provsql','tree_decomposition_dot' LANGUAGE C;

/**
 * @brief Report whether an external tool is on the backend's resolved PATH
 *
 * Uses the same @c find_external_tool() helper that the compilers
 * (d4 / c2d / minic2d / dsharp / panini), model counters (ganak /
 * sharpsat-td / dpmc via htb+dmc / weightmc), and visualisation
 * wrappers (graph-easy, dot) themselves consult, so the result
 * reflects exactly what a subsequent @c probability_evaluate or
 * @c view_circuit call would see, including the
 * @c provsql.tool_search_path GUC prepended to @c $PATH.
 *
 * Names with a slash are treated as paths and tested directly via
 * @c access(X_OK); bare names are resolved through @c /bin/sh's
 * @c command -v under the backend's PATH.
 *
 * @param name bare executable (e.g. @c 'd4') or an absolute path
 * @return true iff the tool resolves to an executable file
 */
CREATE OR REPLACE FUNCTION tool_available(name TEXT)
  RETURNS BOOLEAN AS
  'provsql','tool_available' LANGUAGE C STRICT;

/* ----------------------------------------------------------------------
 * External-tool registry
 *
 * A catalog of the external tools ProvSQL can invoke (the knowledge
 * compilers, weighted model counters, and the graph-easy DOT renderer).
 * The default tools and their invocations are compiled in (seeded in C), so
 * out-of-the-box behaviour is unchanged with no configuration.
 *
 * Administrators may add / repoint / reorder / disable tools at run time;
 * those changes are persisted in the @c provsql.tool_overrides table below
 * and overlaid on the compiled seed, so they survive across sessions and
 * backends (and dump/restore).  An empty overrides table means exactly the
 * compiled defaults.  The mutators are superuser-only because a tool record
 * names an executable run as the PostgreSQL OS user (the same trust level as
 * provsql.tool_search_path).
 * ---------------------------------------------------------------------- */

/**
 * @brief Persistent overrides overlaid on the compiled-in tool seed.
 *
 * Each row is the complete desired record for a tool (added or modified) keyed
 * by logical @c name, or a tombstone (@c removed = true) hiding a seeded
 * default.  The effective registry is the compiled seed with tombstoned names
 * removed and the remaining rows upserted over it.  Written only by the
 * superuser-only register_tool / unregister_tool / set_tool_* functions;
 * read back into each backend's in-memory registry on demand.  Marked as a
 * configuration table so pg_dump carries an operator's registrations.
 */
CREATE TABLE IF NOT EXISTS tool_overrides(
  name           TEXT PRIMARY KEY,
  removed        BOOLEAN NOT NULL DEFAULT false,
  kind           TEXT,
  executable     TEXT,
  operations     TEXT[],
  input_formats  TEXT[],
  output_format  TEXT,
  parser         TEXT,
  preference     INT,
  enabled        BOOLEAN,
  dependencies   TEXT[],
  argtpl         TEXT,
  argtpl_circuit TEXT,
  endpoint       TEXT
);
SELECT pg_catalog.pg_extension_config_dump('tool_overrides', '');

/**
 * @brief Set-returning listing backing the @c provsql.tools view.
 *
 * @c operations / @c input_formats / @c output_format use the KCMCP
 * shared-registry names (see the KCMCP server protocol), so a CLI record and
 * a future kcmcp-server record are comparable; @c parser is the CLI-only tag
 * for how to decode the tool's raw output.  @c argtpl is the command template
 * ({in}/{out}/... placeholders).  @c available is true iff @c executable
 * (when set) and every dependency currently resolve on the backend's PATH.
 */
CREATE OR REPLACE FUNCTION tool_registry_list()
  RETURNS TABLE(name TEXT, kind TEXT, executable TEXT, operations TEXT[],
                input_formats TEXT[], output_format TEXT, parser TEXT,
                preference INT, enabled BOOLEAN, argtpl TEXT,
                argtpl_circuit TEXT, endpoint TEXT, available BOOLEAN) AS
  'provsql','tool_registry_list' LANGUAGE C STABLE;

/**
 * @brief Read-only view of the registered tools.
 */
CREATE OR REPLACE VIEW tools AS
  SELECT name, kind, executable, operations, input_formats, output_format,
         parser, preference, enabled, argtpl, argtpl_circuit, endpoint,
         available
  FROM tool_registry_list();

/**
 * @brief Register a tool, or replace the record with the same logical name.
 *
 * @param name        logical id (e.g. @c 'd4-jm62300'); also the value
 *                    @c provsql.fallback_compiler / the wmc tool selector use
 * @param executable  executable to resolve on PATH (defaults to @c name)
 * @param kind          @c 'cli' (spawn @c executable) or @c 'kcmcp' (talk to
 *                      the KCMCP server at @c endpoint)
 * @param operations    capabilities (KCMCP names): @c 'compile' / @c 'wmc'
 *                      (and ProvSQL-local @c 'render')
 * @param input_formats accepted inputs (KCMCP names): @c 'dimacs-cnf',
 *                      @c 'circuit-bcs12' (listing @c 'circuit-bcs12' enables
 *                      the native-circuit fast path)
 * @param output_format result encoding (KCMCP names): @c 'ddnnf-nnf',
 *                      @c 'decimal', @c 'rational', ... (local @c 'panini-dd'
 *                      / @c 'ascii' where KCMCP has no code)
 * @param parser        CLI-only decode tag: @c 'nnf' (the tolerant d4 / c2d
 *                      NNF reader), @c 'panini-dd', @c 'wmc-line',
 *                      @c 'weightmc', @c 'ascii'
 * @param argtpl        command template; placeholders @c {in} / @c {out}
 *                      (and @c {binary} / @c {tmpdir} / @c {pivotAC}).  When
 *                      it omits @c {binary}, the executable is prepended.
 * @param argtpl_circuit command used when the @c 'circuit-bcs12' input is
 *                      selected (a BC-S1.2 circuit rather than a CNF); only a
 *                      tool accepting that input needs it
 * @param preference    ordering within an operation (higher first)
 * @param enabled       whether the dispatchers may select it
 * @param endpoint      for a @c 'kcmcp' record, the server address:
 *                      @c 'unix:/path' or @c 'host:port'
 *
 * Superuser-only: a CLI record runs an arbitrary command as the PostgreSQL
 * OS user, and a kcmcp record names a socket the server connects to.
 */
CREATE OR REPLACE FUNCTION register_tool(
  name TEXT,
  executable TEXT DEFAULT NULL,
  kind TEXT DEFAULT 'cli',
  operations TEXT[] DEFAULT NULL,
  input_formats TEXT[] DEFAULT NULL,
  output_format TEXT DEFAULT NULL,
  parser TEXT DEFAULT NULL,
  argtpl TEXT DEFAULT NULL,
  argtpl_circuit TEXT DEFAULT NULL,
  preference INT DEFAULT 0,
  enabled BOOLEAN DEFAULT true,
  endpoint TEXT DEFAULT NULL)
  RETURNS void AS
  'provsql','tool_registry_register' LANGUAGE C;

/** @brief Unregister a tool; errors on an unknown tool name. Superuser-only. */
CREATE OR REPLACE FUNCTION unregister_tool(name TEXT)
  RETURNS void AS
  'provsql','tool_registry_unregister' LANGUAGE C STRICT;

/** @brief Enable/disable a tool; errors on an unknown tool name. Superuser-only. */
CREATE OR REPLACE FUNCTION set_tool_enabled(name TEXT, enabled BOOLEAN)
  RETURNS void AS
  'provsql','tool_registry_set_enabled' LANGUAGE C STRICT;

/** @brief Set a tool's preference; errors on an unknown tool name. Superuser-only. */
CREATE OR REPLACE FUNCTION set_tool_preference(name TEXT, preference INT)
  RETURNS void AS
  'provsql','tool_registry_set_preference' LANGUAGE C STRICT;

-- The mutators guard at the C level too, but revoke from PUBLIC so the
-- superuser requirement is visible in the catalog.
REVOKE ALL ON FUNCTION register_tool(TEXT, TEXT, TEXT, TEXT[], TEXT[], TEXT, TEXT, TEXT, TEXT, INT, BOOLEAN, TEXT) FROM PUBLIC;
REVOKE ALL ON FUNCTION unregister_tool(TEXT) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_tool_enabled(TEXT, BOOLEAN) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_tool_preference(TEXT, INT) FROM PUBLIC;

/**
 * @brief Return an XML representation of the provenance circuit
 *
 * @param token root provenance token
 * @param token2desc optional mapping table for gate descriptions
 */
CREATE OR REPLACE FUNCTION to_provxml(
  token UUID,
  token2desc regclass = NULL)
  RETURNS TEXT AS
  'provsql','to_provxml' LANGUAGE C;

/** @brief Return the provenance token of the current query result tuple */
CREATE OR REPLACE FUNCTION provenance() RETURNS UUID AS
 'provsql', 'provenance' LANGUAGE C;

/**
 * @brief Compute where-provenance for a result tuple
 *
 * Returns a text representation showing which input columns
 * contributed to each output column.
 */
CREATE OR REPLACE FUNCTION where_provenance(token UUID)
  RETURNS text AS
  'provsql','where_provenance' LANGUAGE C;

/** @} */

/** @defgroup circuit_init Circuit initialization
 *  Functions and statements executed at extension load time to
 *  reset internal caches and create the constant zero/one gates.
 *  @{
 */

/** @brief Reset the internal cache of OID constants used by the query rewriter */
CREATE OR REPLACE FUNCTION reset_constants_cache()
  RETURNS void AS
  'provsql', 'reset_constants_cache' LANGUAGE C;

SELECT reset_constants_cache();

SELECT create_gate(gate_zero(), 'zero');
SELECT create_gate(gate_one(), 'one');

/** @} */

/** @brief Types of update operations tracked for temporal provenance */
CREATE TYPE query_type_enum AS ENUM (
    'INSERT',  -- Row was inserted
    'DELETE',  -- Row was deleted
    'UPDATE',  -- Row was updated
    'UNDO'     -- Previous operation was undone
    );

/** @defgroup compiled_semirings Compiled semirings
 *  Definitions of compiled semirings
 *  @{
 */

/** @brief Evaluate provenance as a symbolic formula (e.g., "a ⊗ b ⊕ c") */
CREATE FUNCTION sr_formula(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'formula',
    '𝟙'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance over the counting semiring (ℕ) */
CREATE FUNCTION sr_counting(token ANYELEMENT, token2value regclass)
  RETURNS INT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'counting',
    1
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance as why-provenance (set of witness sets) */
CREATE FUNCTION sr_why(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'why',
    '{}'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance as how-provenance (canonical polynomial provenance ℕ[X], universal commutative-semiring provenance) */
CREATE FUNCTION sr_how(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'how',
    '{}'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance as which-provenance (lineage: a single set of contributing labels) */
CREATE FUNCTION sr_which(token ANYELEMENT, token2value regclass)
  RETURNS VARCHAR AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'which',
    '{}'::VARCHAR
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance as a Boolean expression
 *
 * The optional @p token2value mapping labels the leaves of the
 * formula: when omitted, leaves are rendered as bare @c x@<id@>
 * placeholders.
 */
CREATE FUNCTION sr_boolexpr(token ANYELEMENT, token2value regclass = NULL)
  RETURNS VARCHAR AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'boolexpr',
    '⊤'::VARCHAR
  );
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

/** @brief Evaluate provenance over the Boolean semiring (true/false) */
CREATE FUNCTION sr_boolean(token ANYELEMENT, token2value regclass)
  RETURNS BOOLEAN AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'boolean',
    TRUE
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

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

/** @brief Evaluate provenance over the Viterbi (max-times) m-semiring
 *
 * Inputs are read as %float8 probability values in @f$[0,1]@f$.
 * Returns the probability of the most likely derivation.
 */
CREATE FUNCTION sr_viterbi(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'viterbi',
    1::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance over the Łukasiewicz fuzzy m-semiring
 *
 * Inputs are read as %float8 graded-truth values in @f$[0,1]@f$.
 * Addition is @f$\max@f$; multiplication is the Łukasiewicz t-norm
 * @f$\max(a + b - 1, 0)@f$, which preserves crisp truth and avoids
 * the near-zero collapse of long product chains.
 */
CREATE FUNCTION sr_lukasiewicz(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'lukasiewicz',
    1::FLOAT
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance over the min-max m-semiring on a user enum
 *
 * Inputs are read as values of a user-defined enum carrier; addition
 * is enum-min, multiplication is enum-max. Bottom and top of the enum
 * are derived from @c pg_enum.enumsortorder. The third argument is a
 * sample value of the carrier enum, used only for type inference; its
 * value is ignored.
 *
 * The security shape: alternative derivations combine to the least
 * sensitive label, joins combine to the most sensitive label.
 *
 * @param token Provenance token to evaluate.
 * @param token2value Mapping from input gates to enum values.
 * @param element_one Sample value of the carrier enum (any value works).
 */
CREATE FUNCTION sr_minmax(token UUID, token2value regclass, element_one ANYENUM)
  RETURNS ANYENUM AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'minmax',
    element_one
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @brief Evaluate provenance over the max-min m-semiring on a user enum
 *
 * Dual of :sqlfunc:`sr_minmax`: addition is enum-max, multiplication
 * is enum-min. The fuzzy / availability / trust shape: alternatives
 * combine to the most permissive label, joins combine to the strictest
 * label. The third argument is a sample value of the carrier enum,
 * used only for type inference; its value is ignored.
 *
 * @param token Provenance token to evaluate.
 * @param token2value Mapping from input gates to enum values.
 * @param element_one Sample value of the carrier enum (any value works).
 */
CREATE FUNCTION sr_maxmin(token UUID, token2value regclass, element_one ANYENUM)
  RETURNS ANYENUM AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'maxmin',
    element_one
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/** @} */

/** @defgroup choose_aggregate choose aggregate
 *  Choose one value among many, used in particular to code a mutually
 * exclusive choice as an aggregate.
 *  @{
 */

/** @brief Transition function for the choose aggregate (keeps first non-NULL value) */
CREATE FUNCTION choose_function(state ANYELEMENT, data ANYELEMENT)
  RETURNS ANYELEMENT AS
$$
BEGIN
  IF state IS NULL THEN
    RETURN data;
  ELSE
    RETURN state;
  END IF;
END
$$ LANGUAGE plpgsql PARALLEL SAFE IMMUTABLE;

/** @brief Aggregate that returns an arbitrary non-NULL value from a group */
CREATE AGGREGATE choose(ANYELEMENT) (
  SFUNC = choose_function,
  STYPE = ANYELEMENT
);

/** @brief Explodes a table column containing aggregated provenance into multiple rows.
 *
 *  For each row in the input table, this function unnests the children of the
 *  specified aggregate token column and produces one output row per child.
 *  It reconstructs the corresponding value and provenance (`provsql`) for
 *  each resulting row.
 *
 *  The original table is replaced by the transformed table.
 *
 *  @param _tbl Name of the table to transform.
 *  @param agg_token Name of the column containing the aggregate to explode.
 */
CREATE OR REPLACE FUNCTION explode_table(_tbl text, agg_token text)
RETURNS void AS $$
DECLARE
  _nsp text;
BEGIN
    -- Resolve the schema actually holding _tbl so the rebuilt table is
    -- recreated in place (the provsql helper functions are schema-qualified
    -- so this works whatever the caller's search_path is).
    SELECT n.nspname INTO _nsp
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE c.oid = _tbl::regclass;

    EXECUTE format('
    CREATE TABLE %1$I.temp_exploded AS
    SELECT
        %2$I.*,
        provsql.get_extra(children[2]) AS new_t,
        provsql.provenance_times(children[1], provsql) AS new_provsql
    FROM %1$I.%2$I,
    LATERAL (
        SELECT provsql.get_children(sm) AS children
        FROM UNNEST(provsql.get_children(%3$I)) AS sm
    ) AS sub', _nsp, _tbl, agg_token);
    EXECUTE format('DROP TABLE %I.%I', _nsp, _tbl);
    EXECUTE format('ALTER TABLE %I.temp_exploded DROP COLUMN %I, DROP COLUMN provsql', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_t TO %I', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_provsql TO provsql', _nsp);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME TO %I', _nsp, _tbl);
END;
$$ LANGUAGE plpgsql;

/** @} */

/**
 * @brief Append @c provsql to this database's default search_path, if missing.
 *
 * ProvSQL's operators and functions live in the @c provsql schema and
 * are resolved through @c search_path.  When @c provsql is absent from
 * the path some surfaces fail with a clear error (RV/agg_token
 * arithmetic), but others can be silently misrouted by an implicit
 * cross-domain cast.  This helper makes the common case painless: it
 * reads the current <em>database-level</em> search_path setting from
 * @c pg_db_role_setting, appends @c provsql if not already present
 * (never replacing or reordering the existing entries), and applies the
 * result with @c ALTER @c DATABASE.  It is idempotent and emits a
 * @c NOTICE describing what it did.
 *
 * Only @b new sessions pick up the change; the calling session keeps its
 * current path.  Role-level settings (if any) take precedence over the
 * database-level setting and are left untouched.  The caller must be the
 * database owner or a superuser (the privilege model of @c ALTER
 * @c DATABASE).  Returns the resulting search_path value.
 */
CREATE OR REPLACE FUNCTION setup_search_path()
  RETURNS text
  LANGUAGE plpgsql AS $$
DECLARE
  db        text := current_database();
  cfg       text[];
  cur       text;        -- existing database-level search_path value
  new_path  text;
BEGIN
  -- setrole = 0 selects the database-wide default, not a per-role override.
  SELECT s.setconfig INTO cfg
    FROM pg_db_role_setting s
    JOIN pg_database d ON d.oid = s.setdatabase
   WHERE d.datname = db AND s.setrole = 0;

  IF cfg IS NOT NULL THEN
    SELECT substr(e, length('search_path=') + 1) INTO cur
      FROM unnest(cfg) AS e
     WHERE e LIKE 'search_path=%';
  END IF;

  IF cur IS NULL THEN
    -- No database-level search_path at all: install the documented
    -- default with provsql appended.
    new_path := '"$user", public, provsql';
    EXECUTE format('ALTER DATABASE %I SET search_path = %s', db, new_path);
    RAISE NOTICE 'ProvSQL: set search_path = % for database "%" (no previous database-level setting). Only new sessions are affected.',
      new_path, db;
    RETURN new_path;
  END IF;

  -- Already contains provsql as a path element?  Idempotent no-op.
  IF EXISTS (
       SELECT 1 FROM unnest(string_to_array(cur, ',')) AS p
        WHERE btrim(btrim(p), '"') = 'provsql')
  THEN
    RAISE NOTICE 'ProvSQL: search_path for database "%" already contains provsql (= %); no change.',
      db, cur;
    RETURN cur;
  END IF;

  new_path := cur || ', provsql';
  EXECUTE format('ALTER DATABASE %I SET search_path = %s', db, new_path);
  RAISE NOTICE 'ProvSQL: appended provsql to search_path for database "%" (now: %). Only new sessions are affected.',
    db, new_path;
  RETURN new_path;
END;
$$;

GRANT USAGE ON SCHEMA provsql TO PUBLIC;

SET search_path TO public;

-- Installation-time advisory: if provsql is not in the database's default
-- search_path, point the user at setup_search_path().  reset_val reflects
-- the configured session default (postgresql.conf / ALTER DATABASE / ALTER
-- ROLE), unaffected by the SET search_path statements this script ran.
-- CREATE EXTENSION raises client_min_messages to WARNING for the duration
-- of the script, so we lower it around the RAISE NOTICE. SET LOCAL only:
-- it unwinds by itself when CREATE EXTENSION's transaction ends. An
-- explicit save/restore here would capture the WARNING clamp (already in
-- force when this block runs) and restore *that* at session level,
-- leaving the whole installing session with NOTICEs suppressed.
DO $$
DECLARE
  rp text;
  has_provsql boolean;
BEGIN
  SELECT reset_val INTO rp FROM pg_settings WHERE name = 'search_path';
  SELECT bool_or(btrim(btrim(p), '"') = 'provsql')
    INTO has_provsql
    FROM unnest(string_to_array(coalesce(rp, ''), ',')) AS p;
  IF NOT coalesce(has_provsql, false) THEN
    SET LOCAL client_min_messages = notice;
    RAISE NOTICE 'ProvSQL: schema "provsql" is not in your default search_path (currently: %).', rp;
    RAISE NOTICE 'ProvSQL operators and functions are resolved through search_path. Run "SELECT provsql.setup_search_path();" to add it, or set it manually (e.g. ALTER DATABASE % SET search_path = "$user", public, provsql).', quote_ident(current_database());
  END IF;
END;
$$;

-- Final constants-cache refresh.  The planned SELECT statements earlier in
-- this script (reset_constants_cache itself, the zero/one create_gate calls)
-- make the installing session memoize the OID constants *mid-script*, while
-- objects defined later (notably the choose aggregate, used by the
-- scalar-subquery decorrelation) do not exist yet.  Their optional lookups
-- then stay InvalidOid for the rest of the session, silently disabling the
-- corresponding rewrites (e.g. IN/NOT IN over a tracked relation would raise
-- "Subqueries ... not supported") until a new connection.  Refreshing here,
-- after every object exists, repairs the installing session's cache.
SELECT provsql.reset_constants_cache();
