/**
 * @file
 * @brief ProvSQL upgrade script: 1.5.0 -> 1.6.0
 *
 * 1.6.0 introduces the safe-query (Boolean-rewrite) optimisation
 * controlled by the @c provsql.boolean_provenance GUC.  The on-disk
 * gate enum gains one value, the SQL surface gains a handful of new
 * functions, and every existing @c add_provenance / @c repair_key
 * table is migrated to the new no-UNIQUE / no-DEFAULT shape that the
 * @c provenance_guard trigger relies on.  Concretely:
 *
 * - One new gate type: @c assumed_boolean (single-child structural
 *   marker; transparent for Boolean-compatible evaluators, fatal for
 *   the rest).  Appended to the @c provenance_gate enum.
 *
 * - Per-table provenance metadata, backed by a new
 *   @c provsql_table_info.mmap file (the background worker creates
 *   it lazily on first use).  Three SQL bindings: @c set_table_info,
 *   @c remove_table_info, @c get_table_info.
 *
 * - @c cleanup_table_info event trigger that purges stale metadata
 *   when a tracked relation is dropped outside @c remove_provenance.
 *
 * - @c assume_boolean(uuid) constructor for @c gate_assumed_boolean.
 *   Used by the safe-query rewriter; reusable from any Boolean-only
 *   simplification that follows.
 *
 * - @c provenance_guard() trigger function: fills @c NEW.provsql with
 *   a fresh @c uuid_generate_v4 leaf when omitted, and flips the
 *   table to @c OPAQUE when the user supplies their own value.
 *
 * - Replacement bodies for @c add_provenance (both the common and
 *   PG14+ overrides), @c remove_provenance, and @c repair_key.  The
 *   per-table column now has no @c UNIQUE constraint and no column
 *   @c DEFAULT (the trigger does both jobs), and carries a plain
 *   index for fast UUID-keyed lookups.
 *
 * The script also migrates every existing tracked relation in place
 * (drops the old @c UNIQUE constraint, drops the @c DEFAULT, creates
 * a plain index if the unique one was the only one, and installs the
 * @c provenance_guard trigger) and seeds the table-info store with
 * a @c TID entry for each.
 *
 * @warning Users who applied @c repair_key in 1.5.0 to mark a table
 * as BID lose that classification across the upgrade: the seeding
 * loop has no way to recover the @c block_key columns from the
 * existing mmap circuit.  Re-run @c repair_key after the upgrade for
 * every BID-shaped table to restore the @c BID metadata; otherwise
 * the safe-query rewriter will treat those tables as @c TID and
 * silently produce unsound results under
 * @c provsql.boolean_provenance.
 *
 * Idempotency: every @c CREATE @c FUNCTION uses @c OR @c REPLACE;
 * the event trigger is dropped and recreated; the per-table
 * migration loop checks @c pg_constraint / @c pg_trigger /
 * @c pg_index before each step so re-running the script is safe.
 *
 * @warning ABI: @c assumed_boolean is appended at the end of the C
 * @c gate_type enum (@c src/provsql_utils.h), preserving every
 * existing integer-to-name mapping.  1.5.0 mmap circuits remain
 * valid; no on-disk migration required.
 */

SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. New gate-type enum value, in the same order as the C enum.
-- ----------------------------------------------------------------------

ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'assumed_boolean'
  AFTER 'mixture';

-- ----------------------------------------------------------------------
-- 2. New SQL bindings for the per-relation metadata store.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION set_table_info(
  relid OID, kind TEXT, block_key INT2[] DEFAULT ARRAY[]::INT2[])
  RETURNS void AS
  'provsql','set_table_info' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION remove_table_info(relid OID)
  RETURNS void AS
  'provsql','remove_table_info' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION get_table_info(
  relid OID, OUT kind TEXT, OUT block_key INT2[])
  RETURNS record AS
  'provsql','get_table_info' LANGUAGE C STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION set_ancestors(
  relid OID, ancestors OID[] DEFAULT ARRAY[]::OID[])
  RETURNS void AS
  'provsql','set_ancestors' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION remove_ancestors(relid OID)
  RETURNS void AS
  'provsql','remove_ancestors' LANGUAGE C PARALLEL SAFE;

CREATE OR REPLACE FUNCTION get_ancestors(relid OID)
  RETURNS OID[] AS
  'provsql','get_ancestors' LANGUAGE C STABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 3. Event trigger that purges per-table metadata when a tracked
--    relation is dropped without going through remove_provenance.
-- ----------------------------------------------------------------------

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
CREATE EVENT TRIGGER provsql_cleanup_table_info ON sql_drop
  EXECUTE PROCEDURE provsql.cleanup_table_info();

-- ----------------------------------------------------------------------
-- 4. assume_boolean: gate_assumed_boolean constructor.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION assume_boolean(token UUID) RETURNS UUID AS
$$
DECLARE
  wrapped uuid;
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  wrapped := public.uuid_generate_v5(uuid_ns_provsql(),
                                     concat('assumed_boolean', token));
  PERFORM create_gate(wrapped, 'assumed_boolean', ARRAY[token]);
  RETURN wrapped;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 5. provenance_guard: BEFORE INSERT OR UPDATE OF provsql trigger.
-- ----------------------------------------------------------------------

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

-- ----------------------------------------------------------------------
-- 6. Replacement bodies for add_provenance / remove_provenance /
--    repair_key.  The PG14+ add_provenance override (which also
--    installs the update_provenance AFTER triggers) is replaced
--    further down inside a server-version guard.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
  EXECUTE format(
    'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
    _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE FUNCTION provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'tid');
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION remove_provenance(_tbl regclass)
  RETURNS void AS
$$
DECLARE
BEGIN
  PERFORM provsql.remove_table_info(_tbl::oid);
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

CREATE OR REPLACE FUNCTION repair_key(_tbl regclass, key_att text)
  RETURNS void AS
$$
DECLARE
  r RECORD;
  rows_query TEXT;
  block_key_cols INT2[];
BEGIN
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

  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql_temp UUID DEFAULT public.uuid_generate_v4()', _tbl);

  IF key_att = '' THEN
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %s', _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              k.provsql_group_size AS group_size,
              row_number() OVER ()::int - 1 AS within_group
         FROM %s t, provsql_repair_key_tmp k', _tbl);
  ELSE
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT %1$s,
                public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %2$s GROUP BY %1$s', key_att, _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              k.provsql_group_size AS group_size,
              (row_number() OVER (PARTITION BY %1$s))::int - 1 AS within_group
         FROM %2$s t JOIN provsql_repair_key_tmp k USING (%1$s)',
      key_att, _tbl);
  END IF;

  FOR r IN SELECT provsql_key_token FROM provsql_repair_key_tmp LOOP
    PERFORM provsql.create_gate(r.provsql_key_token, 'input');
  END LOOP;

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
    'ON %s FOR EACH ROW EXECUTE FUNCTION provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'bid', block_key_cols);
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- 7. PG14+: replace the data-modification-tracking add_provenance
--    override (the one that also installs the update_provenance
--    AFTER triggers) with the same new shape.
--
--    Gated on the presence of insert_statement_trigger (only
--    installed by sql/provsql.14.sql, itself only loaded when the
--    server is PG14+).  psql meta-commands \gset / \if are not
--    valid inside an extension script -- ALTER EXTENSION runs each
--    script via SPI, not psql -- so the conditional uses a DO
--    block with nested dollar-quoting instead.
-- ----------------------------------------------------------------------

DO $migrate$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_proc
     WHERE proname     = 'insert_statement_trigger'
       AND pronamespace = 'provsql'::regnamespace
  ) THEN
    EXECUTE $ex$
      CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
        RETURNS void AS
      $body$
      BEGIN
        EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
        EXECUTE format(
          'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
          _tbl);
        EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
        EXECUTE format(
          'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
          'ON %s FOR EACH ROW EXECUTE FUNCTION provsql.provenance_guard()',
          _tbl);

        EXECUTE format('CREATE TRIGGER insert_statement AFTER INSERT ON %s REFERENCING NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.insert_statement_trigger()', _tbl);
        EXECUTE format('CREATE TRIGGER delete_statement AFTER DELETE ON %s REFERENCING OLD TABLE AS OLD_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.delete_statement_trigger()', _tbl);
        EXECUTE format('CREATE TRIGGER update_statement AFTER UPDATE ON %s REFERENCING OLD TABLE AS OLD_TABLE NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.update_statement_trigger()', _tbl);

        PERFORM provsql.set_table_info(_tbl::oid, 'tid');
        PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
      END
      $body$ LANGUAGE plpgsql SECURITY DEFINER;
    $ex$;
  END IF;
END
$migrate$;

-- ----------------------------------------------------------------------
-- 8. Per-existing-tracked-table migration.
--    For every relation with a provsql UUID column:
--      a. Drop the auto-named UNIQUE constraint (if it still exists).
--      b. Drop the column DEFAULT (so future user INSERTs that omit
--         the column arrive at the guard with NEW.provsql IS NULL).
--      c. Make sure a plain index on (provsql) exists (the dropped
--         UNIQUE took its underlying index with it).
--      d. Install the provenance_guard BEFORE INSERT/UPDATE trigger.
--      e. Seed a TID entry in the per-database table-info store.
--
--    Users who applied @c repair_key in 1.5.0 are seeded as TID
--    here -- they MUST re-run @c repair_key to restore the BID
--    classification (the script has no way to recover the original
--    block_key columns from the existing mmap circuit).
--      f. Seed the base-ancestor set to {self}.  Base TID/BID tables
--         have themselves as their sole ancestor; CTAS-derived
--         relations the user may have explicitly tracked via
--         @c add_provenance also land at {self} here (a CTAS-source-
--         aware hook is a separate slice -- before it ships, all
--         tracked relations are treated as base for the disjoint-
--         ancestor analysis).
-- ----------------------------------------------------------------------

DO $$
DECLARE
  r       RECORD;
  cname   text;
  has_ix  bool;
  has_trg bool;
BEGIN
  FOR r IN
    SELECT c.oid AS relid, n.nspname, c.relname
      FROM pg_class c
      JOIN pg_attribute a ON a.attrelid = c.oid
      JOIN pg_namespace n ON c.relnamespace = n.oid
     WHERE a.attname     = 'provsql'
       AND a.atttypid    = 'uuid'::regtype
       AND NOT a.attisdropped
       AND c.relkind     = 'r'
       AND n.nspname NOT IN ('pg_catalog', 'information_schema')
  LOOP
    -- (a) drop the auto-generated UNIQUE constraint on provsql.
    SELECT con.conname INTO cname
      FROM pg_constraint con
      JOIN pg_attribute a
        ON a.attrelid = con.conrelid
       AND a.attnum   = ANY(con.conkey)
     WHERE con.conrelid = r.relid
       AND con.contype  = 'u'
       AND a.attname    = 'provsql'
       AND array_length(con.conkey, 1) = 1
     LIMIT 1;
    IF cname IS NOT NULL THEN
      EXECUTE format('ALTER TABLE %I.%I DROP CONSTRAINT %I',
                     r.nspname, r.relname, cname);
    END IF;

    -- (b) drop the column DEFAULT (uuid_generate_v4()).
    EXECUTE format('ALTER TABLE %I.%I ALTER COLUMN provsql DROP DEFAULT',
                   r.nspname, r.relname);

    -- (c) recreate a plain index if one is missing.
    SELECT EXISTS (
      SELECT 1 FROM pg_index i
        JOIN pg_attribute a
          ON a.attrelid = i.indrelid
         AND a.attnum   = i.indkey[0]
       WHERE i.indrelid = r.relid
         AND i.indnatts = 1
         AND a.attname  = 'provsql'
    ) INTO has_ix;
    IF NOT has_ix THEN
      EXECUTE format('CREATE INDEX ON %I.%I(provsql)',
                     r.nspname, r.relname);
    END IF;

    -- (d) install the provenance_guard trigger.
    SELECT EXISTS (
      SELECT 1 FROM pg_trigger
       WHERE tgrelid = r.relid
         AND tgname  = 'provenance_guard'
         AND NOT tgisinternal
    ) INTO has_trg;
    IF NOT has_trg THEN
      EXECUTE format(
        'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
        'ON %I.%I FOR EACH ROW EXECUTE FUNCTION provsql.provenance_guard()',
        r.nspname, r.relname);
    END IF;

    -- (e) seed table-info as TID.  Users who repair_key'd in 1.5.0
    --     must re-run repair_key after the upgrade to restore BID.
    PERFORM provsql.set_table_info(r.relid, 'tid');

    -- (f) seed the base-ancestor set to {self}.
    PERFORM provsql.set_ancestors(r.relid, ARRAY[r.relid]);
  END LOOP;
END $$;

-- ----------------------------------------------------------------------
-- 9. Invalidate the per-session OID constants cache so the new
--    'assumed_boolean' enum value and the new function OIDs are
--    visible to any backend that warmed its cache under 1.5.0.
-- ----------------------------------------------------------------------

SELECT reset_constants_cache();
