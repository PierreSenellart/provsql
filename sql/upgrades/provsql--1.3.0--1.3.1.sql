/**
 * @file
 * @brief ProvSQL upgrade script: 1.3.0 → 1.3.1
 *
 * 1.3.1 introduces no new SQL surface itself, but the 1.2.3 → 1.3.0
 * upgrade script that shipped with 1.3.0 was incomplete: SQL changes
 * from commits 1f59032 (schema-qualified provsql.time_validity_view in
 * temporal functions) and f670b7f (lazy input gate creation) had
 * already landed in `provsql.common.sql` / `provsql.14.sql` for
 * fresh installs of 1.3.0, but were not propagated to the upgrade
 * script.  Users who installed 1.3.0 fresh have the right SQL
 * surface; users who came through the broken upgrade do not.
 *
 * This script applies the missing changes so users on 1.3.0 reach a
 * clean 1.3.1 SQL surface regardless of how they got to 1.3.0.  Fresh
 * installs of 1.3.0 will also run this script, but every CREATE OR
 * REPLACE here matches the 1.3.0 source already on disk, so it is
 * a no-op for them.
 */
SET search_path TO provsql;

-- 1. Lazy input gates: drop the per-row add_gate triggers from any
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

-- 2. Replace add_provenance().  PG14+ and pre-14 bodies differ.

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

-- 3. Replace repair_key() with the trigger-free body.

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

-- 4. Schema-qualify provsql.time_validity_view in temporal functions
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
