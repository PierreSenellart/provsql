/**
 * @file
 * @brief ProvSQL upgrade script: 1.1.0 → 1.2.0
 *
 * Changes in this release that affect the SQL API:
 *
 * - @c provenance_cmp gained @c SET @c search_path=provsql,pg_temp,public
 *   and @c SECURITY @c DEFINER, so it can be called from contexts where
 *   the current @c search_path has not been set up for ProvSQL.
 *
 * - @c provenance_evaluate (the PL/pgSQL fallback) now emits a more
 *   actionable error message when it encounters a gate type it cannot
 *   handle, pointing the user at the compiled semiring evaluators
 *   instead of just saying "Unknown gate type".
 *
 * - @c provenance_aggregate now guards @c array_length with @c COALESCE
 *   so that an empty @c tokens array produces a zero gate instead of
 *   raising on a @c NULL length, and it drops the @c STRICT qualifier
 *   from its SQL declaration (the @c COALESCE fix makes it safe to be
 *   non-strict so that aggregate-result rewrites with empty inputs
 *   still work).
 *
 * All three changes are in-place @c CREATE @c OR @c REPLACE @c FUNCTION
 * rewrites of existing function bodies; none add or remove anything.
 * The upgrade is therefore idempotent on the extension level even
 * though PostgreSQL will only fire it once per 1.1.0 → 1.2.0
 * transition.
 */

SET search_path TO provsql;

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
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',tokens));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid, aggtype);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER;
