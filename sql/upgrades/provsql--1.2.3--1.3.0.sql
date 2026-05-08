/**
 * @file
 * @brief ProvSQL upgrade script: 1.2.3 → 1.3.0
 *
 * Two groups of changes are applied:
 *
 * 1. Storage layout. 1.3.0 changes the circuit storage layout from a
 *    single set of flat files (`$PGDATA/provsql_*.mmap`, shared by all
 *    databases) to per-database files
 *    (`$PGDATA/base/<db_oid>/provsql_*.mmap`).  If old flat files are
 *    still present in `$PGDATA`, the circuit data they contain has not
 *    been migrated and will be inaccessible.  This script raises a
 *    WARNING with recovery instructions in that case.
 *
 *    The correct upgrade procedure is:
 *      1. While the old PostgreSQL is still running, run:
 *           provsql_migrate_mmap -D $PGDATA -c <connstr>
 *         The tool migrates each database's circuit data into the new
 *         per-database files and deletes the old flat files on success.
 *      2. Install the new ProvSQL binaries (make install).
 *      3. Restart PostgreSQL.
 *      4. In each database: ALTER EXTENSION provsql UPDATE;
 *
 * 2. SQL surface changes that landed during the 1.3.0 dev cycle but
 *    were not propagated to this upgrade script when 1.3.0 was first
 *    released. Shipped retroactively in the 1.3.1 release of the
 *    extension, so that users still on 1.2.3 reach a 1.3.0-equivalent
 *    SQL surface when they run ALTER EXTENSION provsql UPDATE.
 *
 *    - Lazy input gate creation: add_provenance() no longer eagerly
 *      writes input gates for existing rows nor installs a per-row
 *      `add_gate` trigger.  The dropped trigger function is removed
 *      and any leftover `add_gate` triggers on user tables are dropped.
 *    - repair_key() likewise stops installing the per-row `add_gate`
 *      trigger.
 *    - Temporal functions (timetravel, timeslice, history,
 *      get_valid_time) schema-qualify the `provsql.time_validity_view`
 *      lookup so they no longer fail when search_path does not
 *      include the provsql schema.
 *
 *    The temporal functions only exist on PostgreSQL 14+, so their
 *    redefinition is guarded by a server_version_num check.
 */
SET search_path TO provsql;

-- 1. Storage-layout WARNING (per-database mmap migration).

DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_ls_dir('.') WHERE pg_ls_dir = 'provsql_gates.mmap'
  ) THEN
    RAISE WARNING
      E'ProvSQL 1.3.0: old flat circuit files detected in the data directory.\n'
      'Circuit data stored in those files has NOT been migrated to the new\n'
      'per-database layout and is currently inaccessible.\n'
      '\n'
      'To recover:\n'
      '  1. Run: provsql_migrate_mmap -D % -c <connstr>\n'
      '     The tool migrates all databases and deletes the old flat files.\n'
      '     Run it before any provenance query executes; if queries have already\n'
      '     run, first delete any empty per-database files:\n'
      '       rm -f %/base/*/provsql_*.mmap\n'
      '     then restart PostgreSQL and run the tool immediately.\n'
      '  2. Restart PostgreSQL so the background worker picks up the migrated files.',
      current_setting('data_directory'),
      current_setting('data_directory');
  END IF;
END
$$;

-- 2. Lazy input gates: drop the per-row add_gate triggers from any
--    table that previously called add_provenance() or repair_key()
--    under 1.2.3, then drop the trigger function itself.

DO $$
DECLARE
  r record;
BEGIN
  FOR r IN
    SELECT n.nspname AS schemaname, c.relname AS tablename
    FROM pg_trigger t
    JOIN pg_class c ON c.oid = t.tgrelid
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE NOT t.tgisinternal AND t.tgname = 'add_gate'
  LOOP
    EXECUTE format('DROP TRIGGER add_gate ON %I.%I',
                   r.schemaname, r.tablename);
  END LOOP;
END
$$;

DROP FUNCTION IF EXISTS add_gate_trigger() CASCADE;

-- 3. Replace add_provenance().  PG14+ and pre-14 bodies differ.

DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 140000 THEN
    EXECUTE $sql$
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$func$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);

  EXECUTE format('CREATE TRIGGER insert_statement AFTER INSERT ON %s REFERENCING NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.insert_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER delete_statement AFTER DELETE ON %s REFERENCING OLD TABLE AS OLD_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.delete_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER update_statement AFTER UPDATE ON %s REFERENCING OLD TABLE AS OLD_TABLE NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.update_statement_trigger()', _tbl);
END
$func$ LANGUAGE plpgsql SECURITY DEFINER;
    $sql$;
  ELSE
    EXECUTE $sql$
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$func$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);
END
$func$ LANGUAGE plpgsql SECURITY DEFINER;
    $sql$;
  END IF;
END
$$;

-- 4. Replace repair_key() with the trigger-free body.

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

-- 5. Schema-qualify provsql.time_validity_view in temporal functions
--    (PostgreSQL 14+ only — the functions don't exist on pre-14).

DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 140000 THEN
    EXECUTE $sql$
CREATE OR REPLACE FUNCTION timetravel(
  tablename text,
  at_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
BEGIN
    RETURN QUERY EXECUTE format(
        '
          SELECT
            %1$I.*,
            union_tstzintervals(provenance(), %2$L)
          FROM
            %1$I
          WHERE
            union_tstzintervals(provenance(), %2$L) @> %3$L::timestamptz
        ',
        tablename,
        'provsql.time_validity_view',
        at_time::text
    );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION timeslice(
  tablename text,
  from_time timestamptz,
  to_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
BEGIN
  RETURN QUERY EXECUTE format(
    '
      SELECT
        %1$I.*,
        union_tstzintervals(provenance(), %2$L)
      FROM
        %1$I
      WHERE
        union_tstzintervals(provenance(), %2$L)
        && tstzrange(%3$L::timestamptz, %4$L::timestamptz)
    ',
    tablename,
    'provsql.time_validity_view',
    from_time::text,
    to_time::text
  );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION history(
  tablename text,
  col_names text[],
  col_values text[]
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$func$
DECLARE
    condition text := '';
    i         int;
BEGIN
    IF array_length(col_names, 1) IS NULL
       OR array_length(col_values, 1) IS NULL
       OR array_length(col_names, 1) != array_length(col_values, 1)
    THEN
        RAISE EXCEPTION 'col_names and col_values must have the same (non-null) length';
    END IF;

    FOR i IN 1..array_length(col_names, 1)
    LOOP
        IF i > 1 THEN
            condition := condition || ' AND ';
        END IF;
        condition := condition || format('%I = %L', col_names[i], col_values[i]);
    END LOOP;

    RETURN QUERY EXECUTE format(
      '
        SELECT
          %I.*,
          union_tstzintervals(provenance(), %L)
        FROM
          %I
        WHERE
          %s
      ',
      tablename,
      'provsql.time_validity_view',
      tablename,
      condition
    );
END;
$func$;
    $sql$;

    EXECUTE $sql$
CREATE OR REPLACE FUNCTION get_valid_time(
  token uuid,
  tablename text
)
RETURNS tstzmultirange
LANGUAGE plpgsql
AS $func$
DECLARE
  result tstzmultirange;
BEGIN
  EXECUTE format(
    '
      SELECT
        union_tstzintervals(provenance(), %L)
      FROM
        %I
      WHERE
        provsql = %L
    ',
    'provsql.time_validity_view',
    tablename,
    token
  )
  INTO result;

  RETURN result;
END;
$func$;
    $sql$;
  END IF;
END
$$;
