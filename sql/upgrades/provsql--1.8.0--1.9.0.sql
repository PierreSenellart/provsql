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

-- 4. Refresh the cached OID lookups so a backend warmed under 1.8.0 picks
--    up the new surface on its next get_constants() call.
SELECT reset_constants_cache();
