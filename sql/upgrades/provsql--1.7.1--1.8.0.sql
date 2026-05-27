-- ----------------------------------------------------------------------
-- provsql 1.7.1 -> 1.8.0
--
-- New SQL surface since 1.7.1:
--   * provenance_gate enum value 'annotation' (inversion-free / annotation
--     wrapper gate);
--   * annotate(uuid, text) and inversion_free_key(text, text, int), the
--     SQL surface of the inversion-free d-DNNF compilation path;
--   * the external-tool registry: the tool_overrides configuration table,
--     the tools view, tool_registry_list() and the superuser-only
--     register_tool / unregister_tool / set_tool_enabled /
--     set_tool_preference mutators.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- 1. New gate type.  Append-only; IF NOT EXISTS keeps this script
--    idempotent.  The value is only ADDED here, never USED in this same
--    script (ALTER TYPE ADD VALUE cannot be referenced in the transaction
--    that adds it); annotate() below references it only at call time.
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'annotation' AFTER 'assumed_boolean';

-- 2. Inversion-free SQL surface.

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

CREATE OR REPLACE FUNCTION inversion_free_key(root TEXT, sec TEXT, factor INT)
  RETURNS TEXT AS
$$ SELECT 'K' || factor::text || ' '
       || octet_length(root) || ':' || root
       || octet_length(sec)  || ':' || sec $$
  LANGUAGE sql IMMUTABLE PARALLEL SAFE;

-- 3. External-tool registry.

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

CREATE OR REPLACE FUNCTION tool_registry_list()
  RETURNS TABLE(name TEXT, kind TEXT, executable TEXT, operations TEXT[],
                input_formats TEXT[], output_format TEXT, parser TEXT,
                preference INT, enabled BOOLEAN, argtpl TEXT,
                argtpl_circuit TEXT, endpoint TEXT, available BOOLEAN) AS
  'provsql','tool_registry_list' LANGUAGE C STABLE;

CREATE OR REPLACE VIEW tools AS
  SELECT name, kind, executable, operations, input_formats, output_format,
         parser, preference, enabled, argtpl, argtpl_circuit, endpoint,
         available
  FROM tool_registry_list();

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

CREATE OR REPLACE FUNCTION unregister_tool(name TEXT)
  RETURNS void AS
  'provsql','tool_registry_unregister' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION set_tool_enabled(name TEXT, enabled BOOLEAN)
  RETURNS void AS
  'provsql','tool_registry_set_enabled' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION set_tool_preference(name TEXT, preference INT)
  RETURNS void AS
  'provsql','tool_registry_set_preference' LANGUAGE C STRICT;

-- The mutators guard at the C level too, but revoke from PUBLIC so the
-- superuser requirement is visible in the catalog.
REVOKE ALL ON FUNCTION register_tool(TEXT, TEXT, TEXT, TEXT[], TEXT[], TEXT, TEXT, TEXT, TEXT, INT, BOOLEAN, TEXT) FROM PUBLIC;
REVOKE ALL ON FUNCTION unregister_tool(TEXT) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_tool_enabled(TEXT, BOOLEAN) FROM PUBLIC;
REVOKE ALL ON FUNCTION set_tool_preference(TEXT, INT) FROM PUBLIC;

-- 4. A new provenance_gate enum value was appended above, so a backend
--    warmed under 1.7.1 still caches InvalidOid for it.  Force a fresh
--    look-up on the next get_constants() call.
SELECT reset_constants_cache();
