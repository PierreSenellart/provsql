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
    'mixture'  -- Probabilistic mixture of two scalar RV roots with a Bernoulli weight
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
 * @brief Enable provenance tracking on an existing table
 *
 * Adds a <tt>provsql</tt> UUID column to the table. Input gates for
 * existing rows are created lazily when first referenced by a query.
 *
 * @param _tbl the table to add provenance tracking to
 */
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);
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
  key RECORD;
  key_token uuid;
  token uuid;
  record RECORD;
  nb_rows INTEGER;
  ind INTEGER;
  select_key_att TEXT;
  where_condition TEXT;
BEGIN
  IF key_att = '' THEN
    key_att := '()';
    select_key_att := '1';
  ELSE
    select_key_att := key_att;
  END IF;

  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql_temp UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);

  FOR key IN
    EXECUTE format('SELECT %s AS key FROM %s GROUP BY %s', select_key_att, _tbl, key_att)
  LOOP
    IF key_att = '()' THEN
      where_condition := '';
    ELSE
      where_condition := format('WHERE %s = %L', key_att, key.key);
    END IF;

    EXECUTE format('SELECT COUNT(*) FROM %s %s', _tbl, where_condition) INTO nb_rows;

    key_token := public.uuid_generate_v4();
    PERFORM provsql.create_gate(key_token, 'input');
    ind := 1;
    FOR record IN
      EXECUTE format('SELECT provsql_temp FROM %s %s', _tbl, where_condition)
    LOOP
      token:=record.provsql_temp;
      PERFORM provsql.create_gate(token, 'mulinput', ARRAY[key_token]);
      PERFORM provsql.set_prob(token, 1./nb_rows);
      PERFORM provsql.set_infos(token, ind);
      ind := ind + 1;
    END LOOP;
  END LOOP;
  EXECUTE format('ALTER TABLE %s RENAME COLUMN provsql_temp TO provsql', _tbl);
END
$$ LANGUAGE plpgsql;

/**
 * @brief Create a provenance mapping table from an attribute
 *
 * Creates a new table mapping provenance tokens to values of a given
 * attribute, for use with semiring evaluation functions.
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
 */
CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
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
      times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

      PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
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
 * are trivial, or a single token if only one remains.
 */
CREATE OR REPLACE FUNCTION provenance_plus(tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
  filtered_tokens uuid[];
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
    plus_token := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('plus', filtered_tokens));

    PERFORM create_gate(plus_token, 'plus', filtered_tokens);
  END IF;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

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
    IF result IS NOT NULL THEN
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
      SELECT f, t::uuid, idx, gate_type, NULL AS table_name, NULL AS nb_columns FROM transitive_closure
      UNION ALL
        SELECT DISTINCT t, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM transitive_closure JOIN (SELECT t AS prov, provsql.identify_token(t) as id FROM transitive_closure WHERE t NOT IN (SELECT f FROM transitive_closure)) temp ON t=prov
      UNION ALL
        SELECT DISTINCT $1, NULL::uuid, NULL::int, 'input'::provenance_gate, (id).table_name, (id).nb_columns FROM (SELECT provsql.identify_token($1) AS id WHERE $1 NOT IN (SELECT f FROM transitive_closure)) temp
      ) t
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
  -- Each node's canonical depth is its shortest-path distance from the root.
  -- Tie-breaking on child_pos is irrelevant for the depth value but kept so
  -- the (now informational) row order is stable.
  node_depth AS (
    SELECT node, MIN(depth) AS depth FROM bfs GROUP BY node
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
 */
CREATE OR REPLACE FUNCTION rv_histogram(
  token UUID, bins INT DEFAULT 30) RETURNS jsonb
  AS 'provsql','rv_histogram'
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
 * to the computed scalar). Returns @c NULL if @p token does not
 * resolve to an @c agg gate.
 *
 * @param token UUID of an @c agg gate (typically obtained from an
 *              @c agg_token cell when @c aggtoken_text_as_uuid is on,
 *              or via a manual UUID cast otherwise).
 */
CREATE OR REPLACE FUNCTION agg_token_value_text(token UUID)
  RETURNS text AS
$$
  SELECT CASE
    WHEN provsql.get_gate_type(token) = 'agg'
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

/** @brief Implicit PostgreSQL cast from agg_token to numeric (enables arithmetic on aggregates) */
CREATE CAST (agg_token AS numeric) WITH FUNCTION agg_token_to_numeric(agg_token) AS IMPLICIT;
/** @brief Assignment cast from agg_token to double precision */
CREATE CAST (agg_token AS double precision) WITH FUNCTION agg_token_to_float8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to integer */
CREATE CAST (agg_token AS integer) WITH FUNCTION agg_token_to_int4(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to bigint */
CREATE CAST (agg_token AS bigint) WITH FUNCTION agg_token_to_int8(agg_token) AS ASSIGNMENT;
/** @brief Assignment cast from agg_token to text (extracts value, not UUID) */
CREATE CAST (agg_token AS text) WITH FUNCTION agg_token_to_text(agg_token) AS ASSIGNMENT;

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

/** @} */

/** @defgroup random_variable_type Type for continuous random variables
 *
 *  Custom type <tt>random_variable</tt> that pairs a provenance gate
 *  UUID with a cached scalar value, used to expose continuous
 *  probabilistic c-tables in SQL.  The UUID indexes either a
 *  <tt>gate_rv</tt> (an actual distribution) or a <tt>gate_value</tt>
 *  (a zero-variance constant produced by <tt>provsql.as_random</tt>);
 *  in both cases the cached scalar is convenient for callers that
 *  want the literal without re-parsing the gate's <tt>extra</tt> field.
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
  internallength = 24,
  input  = random_variable_in,
  output = random_variable_out,
  alignment = double
);

/** @brief Extract the provenance UUID part of a random_variable */
CREATE OR REPLACE FUNCTION random_variable_uuid(rv random_variable)
  RETURNS uuid
  AS 'provsql','random_variable_uuid' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Extract the cached scalar value of a random_variable */
CREATE OR REPLACE FUNCTION random_variable_value(rv random_variable)
  RETURNS double precision
  AS 'provsql','random_variable_value' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a random_variable from a UUID and a cached value (internal) */
CREATE OR REPLACE FUNCTION random_variable_make(tok uuid, val double precision)
  RETURNS random_variable
  AS 'provsql','random_variable_make' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Implicit cast random_variable -> uuid (for provsql columns) */
CREATE CAST (random_variable AS uuid)
  WITH FUNCTION random_variable_uuid(random_variable) AS IMPLICIT;

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
 * pointing at it.  The cached scalar is @c NaN.
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
  RETURN provsql.random_variable_make(token, 'NaN'::double precision);
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
 * @c normal above.
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
  RETURN provsql.random_variable_make(token, 'NaN'::double precision);
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
 * @c normal above.
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
  RETURN provsql.random_variable_make(token, 'NaN'::double precision);
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
 * @c normal above.
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
  RETURN provsql.random_variable_make(token, 'NaN'::double precision);
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
 * - @p p must point to a @c gate_input gate with a probability in
 *   [0, 1].  A fresh @c gate_input has default probability 1.0 (so the
 *   mixture deterministically selects @p x); call @c set_prob to pin a
 *   different mixing weight.
 * - @p x and @p y must be scalar RV roots; aggregate / Boolean roots
 *   are rejected at construction.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @c normal above.  Two calls to @c mixture with the same operands
 * are *independent* mixtures with their own gate UUID.  Note that the
 * Bernoulli token @p p, by contrast, is *not* re-minted -- sharing it
 * across calls is exactly how the user couples branch selection.
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
  pi double precision;
BEGIN
  p_kind := provsql.get_gate_type(p);
  IF p_kind <> 'input' THEN
    RAISE EXCEPTION 'provsql.mixture: p must point to a gate_input Bernoulli token (got %)', p_kind;
  END IF;
  pi := provsql.get_prob(p);
  IF pi IS NULL OR pi <> pi OR pi < 0 OR pi > 1 THEN
    RAISE EXCEPTION 'provsql.mixture: p must have a probability in [0,1] set via set_prob (got %)', pi;
  END IF;

  x_uuid := provsql.random_variable_uuid(x);
  y_uuid := provsql.random_variable_uuid(y);
  x_kind := provsql.get_gate_type(x_uuid);
  y_kind := provsql.get_gate_type(y_uuid);
  IF x_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: x must be a scalar RV root (rv / value / arith / mixture), got %', x_kind;
  END IF;
  IF y_kind NOT IN ('rv','value','arith','mixture') THEN
    RAISE EXCEPTION 'provsql.mixture: y must be a scalar RV root (rv / value / arith / mixture), got %', y_kind;
  END IF;

  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'mixture', ARRAY[p, x_uuid, y_uuid]);
  RETURN provsql.random_variable_make(token, 'NaN'::double precision);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Lift a deterministic constant into a random_variable
 *
 * Creates a <tt>gate_value</tt> carrying the constant's text form so
 * that comparisons against a <tt>random_variable</tt> column produce
 * the same circuit shape regardless of whether the operand is an
 * actual RV or a literal constant.  The cached scalar is the constant.
 *
 * Marked <tt>IMMUTABLE</tt>: the gate UUID is derived deterministically
 * from the constant via the same v5 convention as <tt>provenance_semimod</tt>'s
 * inline value gate (<tt>concat('value', CAST(c AS VARCHAR))</tt>), so
 * <tt>as_random(2)</tt> always resolves to the same gate, and any other
 * code path that already creates a value gate for the same constant
 * (e.g. <tt>provenance_semimod</tt>) shares the UUID.
 * <tt>create_gate</tt> is idempotent on already-mapped tokens, so
 * repeat invocations are harmless.
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
  RETURN provsql.random_variable_make(token, c_canon);
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
 * also work — PostgreSQL's operator resolution does not chain casts
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
 * and return a new @c random_variable whose cached scalar is
 * @c NaN (the cached scalar is meaningful only on @c gate_rv leaves
 * and @c gate_value constants from @c as_random).
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
      ARRAY[provsql.random_variable_uuid(a),
            provsql.random_variable_uuid(b)]),
    'NaN'::double precision);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable - @c random_variable (gate_arith MINUS). */
CREATE OR REPLACE FUNCTION random_variable_minus(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      2,  -- PROVSQL_ARITH_MINUS
      ARRAY[provsql.random_variable_uuid(a),
            provsql.random_variable_uuid(b)]),
    'NaN'::double precision);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable * @c random_variable (gate_arith TIMES). */
CREATE OR REPLACE FUNCTION random_variable_times(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      1,  -- PROVSQL_ARITH_TIMES
      ARRAY[provsql.random_variable_uuid(a),
            provsql.random_variable_uuid(b)]),
    'NaN'::double precision);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c random_variable / @c random_variable (gate_arith DIV). */
CREATE OR REPLACE FUNCTION random_variable_div(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- PROVSQL_ARITH_DIV
      ARRAY[provsql.random_variable_uuid(a),
            provsql.random_variable_uuid(b)]),
    'NaN'::double precision);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Unary @c -random_variable (gate_arith NEG). */
CREATE OR REPLACE FUNCTION random_variable_neg(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      4,  -- PROVSQL_ARITH_NEG
      ARRAY[provsql.random_variable_uuid(a)]),
    'NaN'::double precision);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Internal helper: float8-comparator OID for a given symbol.
 *
 * Wraps the <tt>'<sym>(double precision,double precision)'::regoperator</tt>
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
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('<'),
    provsql.random_variable_uuid(b));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &le; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_le(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('<='),
    provsql.random_variable_uuid(b));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a = b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_eq(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('='),
    provsql.random_variable_uuid(b));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &lt;&gt; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_ne(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('<>'),
    provsql.random_variable_uuid(b));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &ge; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_ge(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('>='),
    provsql.random_variable_uuid(b));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Build a @c gate_cmp for <tt>a &gt; b</tt> and return its UUID. */
CREATE OR REPLACE FUNCTION rv_cmp_gt(
  a random_variable, b random_variable) RETURNS uuid AS
$$
  SELECT provsql.provenance_cmp(
    provsql.random_variable_uuid(a),
    provsql.random_variable_cmp_oid('>'),
    provsql.random_variable_uuid(b));
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
 */
CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[])
  RETURNS agg_token AS
$$
DECLARE
  c INTEGER;
  agg_tok uuid;
  agg_val varchar;
BEGIN
  c:=COALESCE(array_length(tokens, 1), 0);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    -- aggfnoid must be part of the UUID: SUM(id) and AVG(id) over the
    -- same children would otherwise collapse to a single gate, and
    -- their concurrent set_infos calls would overwrite each other's
    -- aggregation operator (resulting in the wrong agg_kind being
    -- read by provsql_having under cross-backend contention).
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',aggfnoid,tokens));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid, aggtype);
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
CREATE OR REPLACE FUNCTION rv_moment(token uuid, k integer, central boolean)
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
    RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation';
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  IF aggregation_function = 'sum' THEN
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

    -- ±Infinity sink: positive probability of "no row included" makes
    -- MIN = +Inf and MAX = -Inf.  For MIN^k that's +Inf for any k>=1;
    -- for MAX^k it's (-1)^k * (+Inf) = ±Inf depending on k's parity.
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT probability_evaluate(provenance_monus(prov, provenance_plus(ARRAY_AGG(tok))))
        FROM tok_value
        INTO total_probability;

    IF total_probability > epsilon() THEN
      total := sign_max * 'Infinity'::float8;
    ELSE
      WITH tok_value AS (
        SELECT (get_children(c))[1] AS tok,
               (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
                 * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
        FROM UNNEST(child_pairs) AS c
      ) SELECT sign_max * SUM(p * power(v, k)) FROM (
          SELECT t1.v AS v,
            probability_evaluate(
              provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                               provenance_plus(ARRAY_AGG(t2.tok))),
              method, arguments) AS p
          FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
          GROUP BY t1.v) tmp
        INTO total;
    END IF;
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
    IF prov <> gate_one() THEN
      RAISE EXCEPTION 'variance(): conditioning on prov is not yet supported for random_variable inputs';
    END IF;
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      provsql.random_variable_uuid(input::random_variable), 2, true);
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
    IF prov <> gate_one() THEN
      RAISE EXCEPTION 'moment(): conditioning on prov is not yet supported for random_variable inputs';
    END IF;
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      provsql.random_variable_uuid(input::random_variable), k, false);
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(input::agg_token, k, prov, method, arguments);
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
CREATE OR REPLACE FUNCTION rv_support(token uuid, OUT lo float8, OUT hi float8)
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
 *   token, including the implicit @c random_variable @c -> @c uuid
 *   cast): routes to @c rv_support, which propagates distribution
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
 * @return Composite (lo, hi) with @c -Infinity / @c +Infinity for
 *         unbounded ends.
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

  -- random_variable has an IMPLICIT cast to uuid, so a single
  -- rv_support call covers both shapes.  rv_support handles
  -- gate_value (point), gate_rv (distribution), gate_arith
  -- (propagated), and falls back to the conservative all-real
  -- interval for any other gate kind.  Conditioning on prov is not
  -- supported (would require restricting the underlying joint
  -- distribution by the indicator of prov, which has no closed form
  -- for the basic distributions we ship).
  IF pg_typeof(input) IN ('random_variable'::regtype, 'uuid'::regtype) THEN
    IF prov <> gate_one() THEN
      RAISE EXCEPTION 'support(): conditioning on prov is not yet supported for circuit-token inputs';
    END IF;
    SELECT r.lo, r.hi INTO lo, hi
      FROM provsql.rv_support(input::uuid) r;
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

    IF aggregation_function = 'sum' THEN
      -- Empty agg_token: SUM is identically 0.
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        lo := 0; hi := 0; RETURN;
      END IF;
      SELECT sum(LEAST(v, 0::float8)), sum(GREATEST(v, 0::float8))
        INTO lo, hi
        FROM (SELECT CAST(get_extra((get_children(c))[2]) AS float8) AS v
              FROM unnest(child_pairs) AS c) sub;
    ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
      -- Empty agg_token: MIN = +Infinity, MAX = -Infinity (the
      -- empty-set conventions used by `expected`).
      IF COALESCE(array_length(child_pairs, 1), 0) = 0 THEN
        IF aggregation_function = 'min' THEN
          lo := 'Infinity'::float8; hi := 'Infinity'::float8;
        ELSE
          lo := '-Infinity'::float8; hi := '-Infinity'::float8;
        END IF;
        RETURN;
      END IF;

      WITH tok_value AS (
        SELECT (get_children(c))[1] AS tok,
               CAST(get_extra((get_children(c))[2]) AS float8) AS v
        FROM UNNEST(child_pairs) AS c
      )
      SELECT min(v), max(v),
             probability_evaluate(
               provenance_monus(prov, provenance_plus(ARRAY_AGG(tok))),
               method, arguments)
        INTO lo, hi, total_probability
        FROM tok_value;

      IF total_probability > epsilon() THEN
        IF aggregation_function = 'min' THEN
          hi := 'Infinity'::float8;
        ELSE
          lo := '-Infinity'::float8;
        END IF;
      END IF;
    ELSE
      RAISE EXCEPTION USING MESSAGE=
        'Cannot compute support for aggregation function ' || aggregation_function;
    END IF;
    RETURN;
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
    IF prov <> gate_one() THEN
      RAISE EXCEPTION 'central_moment(): conditioning on prov is not yet supported for random_variable inputs';
    END IF;
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN provsql.rv_moment(
      provsql.random_variable_uuid(input::random_variable), k, true);
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
 */
CREATE FUNCTION sr_tropical(token ANYELEMENT, token2value regclass)
  RETURNS FLOAT AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'tropical',
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

/** @} */

GRANT USAGE ON SCHEMA provsql TO PUBLIC;

SET search_path TO public;
