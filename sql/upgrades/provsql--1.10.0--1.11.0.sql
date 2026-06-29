-- ----------------------------------------------------------------------
-- provsql 1.10.0 -> 1.11.0
--
-- New SQL surface since 1.10.0:
--   * Maintained provenance mappings.  create_provenance_mapping(...,
--     maintained => true) registers the mapping in the new
--     provenance_mapping_registry; provenance_guard then appends each
--     genuine insert to it (keyed to the freshly minted input token), so
--     the mapping stays current AND survives the provsql rewrites that data
--     modification performs.  This fixes the temporal validity reported for
--     a row after it is deleted/updated: a view-based mapping keyed validity
--     on the live (rewritten) provsql column and so lost the row's original
--     interval, while a maintained mapping keeps it keyed to the original
--     input token (the child a later monus/update gate wraps).
--     cleanup_table_info forgets a mapping when either table is dropped.
--   * create_provenance_mapping_view is removed (superseded by maintained
--     mapping tables).  The base time_validity_view, a plain view over the
--     append-only update_provenance log, is unchanged by this upgrade.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- Registry backing maintained mappings.
CREATE TABLE IF NOT EXISTS provsql.provenance_mapping_registry(
  mapping   oid PRIMARY KEY,
  source    oid NOT NULL,
  attribute name NOT NULL
);
CREATE INDEX IF NOT EXISTS provenance_mapping_registry_source_idx
  ON provsql.provenance_mapping_registry(source);

-- create_provenance_mapping gains the `maintained` argument: a signature
-- change, so drop the old four-argument form before recreating.
DROP FUNCTION IF EXISTS create_provenance_mapping(text, regclass, text, bool);
CREATE OR REPLACE FUNCTION create_provenance_mapping(
  newtbl text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT 'f',
  maintained bool DEFAULT false
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
  IF maintained THEN
    -- Register so genuine inserts into oldtbl keep the mapping current
    -- (see provenance_guard); keyed to the input token, so it survives the
    -- provsql rewrites that data modification performs.
    INSERT INTO provsql.provenance_mapping_registry(mapping, source, attribute)
      VALUES (
        (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
              ELSE to_regclass(newtbl) END)::oid,
        oldtbl::oid, att)
      ON CONFLICT (mapping)
        DO UPDATE SET source = EXCLUDED.source, attribute = EXCLUDED.attribute;
  END IF;
END
$$ LANGUAGE plpgsql;

-- provenance_guard appends to registered mappings on genuine inserts.
CREATE OR REPLACE FUNCTION provenance_guard()
  RETURNS TRIGGER AS $$
DECLARE
  _m RECORD;
BEGIN
  IF TG_OP = 'INSERT' THEN
    IF NEW.provsql IS NULL THEN
      -- A genuine insert: mint a fresh atomic input variable. This is the
      -- one place a new input token is born, so it is also where any
      -- maintained mapping on this table is extended (keyed to that token).
      -- Data-modification re-insertions (INSERT ... SELECT * FROM OLD_TABLE)
      -- carry a supplied provsql and take the ELSE branch, so they are
      -- correctly skipped: the validity stays keyed to the original input,
      -- which is exactly the child a later monus/update gate wraps.
      NEW.provsql := public.uuid_generate_v4();
      FOR _m IN SELECT mapping, attribute
                  FROM provsql.provenance_mapping_registry WHERE source = TG_RELID
      LOOP
        EXECUTE format(
          'INSERT INTO %s(value, provenance) SELECT ($1).%I, $2',
          _m.mapping::regclass, _m.attribute)
          USING NEW, NEW.provsql;
      END LOOP;
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

-- cleanup_table_info also forgets registry entries when a table is dropped.
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
    -- Forget any maintained mapping whose source or mapping table is gone.
    DELETE FROM provsql.provenance_mapping_registry
     WHERE source = r.objid OR mapping = r.objid;
  END LOOP;
END
$$ LANGUAGE plpgsql;

-- The view-based mapping helper is removed (superseded by maintained tables).
DROP FUNCTION IF EXISTS create_provenance_mapping_view(text, regclass, text, bool);
