-- ----------------------------------------------------------------------
-- provsql 1.11.1 -> 1.12.0
--
-- Three functions change body; none changes name or argument types, and
-- no gate type, type, operator, cast or aggregate is added or altered, so
-- a plain CREATE OR REPLACE suffices throughout and the script is
-- idempotent.
--
--   * assume_boolean now tags the wrapper it mints with the sq-rewrite
--     route, so the probability dispatcher can report which route
--     answered a query instead of the generic "independent".  The public
--     untagged form remains provenance_assume(token, 'boolean').
--
--   * sr_formula takes an optional mapping and tolerates a NULL token.
--     The formula pseudo-semiring renders every gate kind, including the
--     measure carriers (random variables, arithmetic, mixtures) that have
--     no leaf mapping at all; requiring one made those circuits
--     unreachable.  Dropping STRICT is what lets the NULL-mapping call
--     through, so the NULL-token guard moves into the body.
--
--   * remove_provenance is idempotent, matching add_provenance and
--     create_provenance_mapping since 1.9.0: removing provenance from a
--     table that never had it is a NOTICE-and-no-op rather than an error.
--
-- Adding a parameter default through CREATE OR REPLACE is allowed (only
-- removing one is not), and so is dropping STRICT, so sr_formula does not
-- need a DROP FUNCTION first -- which matters, since dropping it would
-- cascade to anything a user built on top of it.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. The safe-query rewriter's wrapper carries its route tag
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION assume_boolean(token UUID) RETURNS UUID AS
$$
DECLARE
  wrapped uuid;
BEGIN
  wrapped := provenance_assume(token, 'boolean');
  IF wrapped IS NOT NULL THEN
    -- 1 = PROVSQL_ROUTE_SQ_REWRITE (see provsql_route in src/provsql_utils.h)
    PERFORM set_infos(wrapped, 1, 0);
  END IF;
  RETURN wrapped;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 2. sr_formula: optional mapping, NULL-tolerant
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION sr_formula(token ANYELEMENT,
                                      token2value regclass = NULL)
  RETURNS VARCHAR AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'formula',
    '𝟙'::VARCHAR
  );
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

-- ----------------------------------------------------------------------
-- 3. remove_provenance is idempotent
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  PERFORM provsql.remove_table_info(_tbl::oid);
  -- Idempotence, mirroring add_provenance: removing provenance from a
  -- table that does not have it is a NOTICE-and-no-op, so setup scripts
  -- and notebook cells can be re-run freely.  The metadata strip above
  -- still runs, so a table left half-tracked is cleaned up.
  IF NOT EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = _tbl AND attname = 'provsql' AND NOT attisdropped
  ) THEN
    RAISE NOTICE 'table % does not have provenance tracking', _tbl;
    RETURN;
  END IF;
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
