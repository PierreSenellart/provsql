-- ----------------------------------------------------------------------
-- provsql 1.8.0 -> 1.9.0
--
-- New SQL surface since 1.8.0:
--   * comparison of an aggregate (agg_token) with a text constant:
--     the placeholder procedures agg_token_comp_text / text_comp_agg_token
--     and the = / <> operators between agg_token and text (rewritten by
--     ProvSQL at plan time, like the existing agg_token/numeric operators);
--   * explode_table(text, text), which expands an agg_token column back
--     into one row per aggregated child, recombining value and provenance.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- 1. Placeholder comparison procedures.  Never actually executed: they
--    only let the parser accept agg_token <-> text comparisons, which the
--    ProvSQL query rewriter replaces at plan time.
CREATE OR REPLACE FUNCTION agg_token_comp_text(a agg_token, b text)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison agg_token-text not implemented, should be replaced by ProvSQL behavior';
END;
$$;

CREATE OR REPLACE FUNCTION text_comp_agg_token(a text, b agg_token)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT PARALLEL SAFE
AS $$
BEGIN
  RAISE EXCEPTION 'Comparison text-agg_token not implemented, should be replaced by ProvSQL behavior';
END;
$$;

-- 2. = / <> operators between agg_token and text (both argument orders).
CREATE OPERATOR = (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = =,
  NEGATOR    = <>
);
CREATE OPERATOR = (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = =,
  NEGATOR    = <>
);
CREATE OPERATOR <> (
  LEFTARG    = agg_token,
  RIGHTARG   = text,
  PROCEDURE  = agg_token_comp_text,
  COMMUTATOR = <>,
  NEGATOR    = =
);
CREATE OPERATOR <> (
  LEFTARG    = text,
  RIGHTARG   = agg_token,
  PROCEDURE  = text_comp_agg_token,
  COMMUTATOR = <>,
  NEGATOR    = =
);

-- 3. explode_table: expand an aggregate column into one row per child.
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

-- 4. probability_bounds: cheap lower / upper marginal-probability bounds for a
--    monotone-DNF token (new C function).
CREATE OR REPLACE FUNCTION probability_bounds(
  token UUID,
  OUT lower DOUBLE PRECISION,
  OUT upper DOUBLE PRECISION) AS
  'provsql','probability_bounds' LANGUAGE C STABLE;

-- 5. Aggregate semantics: a NULL input never participates in an aggregate.
--    provenance_semimod now returns NULL for a NULL value (so it builds no
--    semimod gate), and provenance_aggregate drops those NULLs before building
--    the agg gate.  Bodies changed, signatures unchanged (same OIDs, so the
--    cached provenance_semimod / provenance_aggregate OIDs stay valid).
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

-- 6. The probability_benchmark helpers (added in 1.7.0) were removed in 1.9.0;
--    drop them so an upgraded database matches a fresh install.
DROP FUNCTION IF EXISTS probability_benchmark(UUID, INT, TEXT);
DROP FUNCTION IF EXISTS _probability_benchmark_one(UUID, TEXT, TEXT);

-- 7. Refresh the cached OID lookups so a backend warmed under 1.8.0 picks
--    up the new surface on its next get_constants() call.
SELECT reset_constants_cache();
