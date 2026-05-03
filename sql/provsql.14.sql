SET search_path TO provsql;

/** @defgroup update_provenance Update provenance (PostgreSQL 14+)
 *  Extended provenance tracking for INSERT, UPDATE, DELETE, and UNDO
 *  operations, including temporal validity ranges.
 *  @{
 */

/**
 * @brief Table recording the history of INSERT, UPDATE, DELETE, and UNDO operations
 *
 * Each row records one provenance-tracked modification, linking the
 * operation's provenance token to metadata (query text, type, user,
 * timestamp) and the temporal validity range of the affected rows.
 */
CREATE TABLE update_provenance (
  provsql uuid,
  query text,
  query_type query_type_enum,
  username text,
  ts timestamp DEFAULT CURRENT_TIMESTAMP,
  valid_time tstzmultirange DEFAULT tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL))
);

/** @cond INTERNAL */
/* Enable provenance tracking on an existing table (PostgreSQL 14+ version).
 * Overrides the common version; documented via add_provenance in provsql.common.sql. */
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID UNIQUE DEFAULT public.uuid_generate_v4()', _tbl);

  EXECUTE format('CREATE TRIGGER insert_statement AFTER INSERT ON %s REFERENCING NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.insert_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER delete_statement AFTER DELETE ON %s REFERENCING OLD TABLE AS OLD_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.delete_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER update_statement AFTER UPDATE ON %s REFERENCING OLD TABLE AS OLD_TABLE NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.update_statement_trigger()', _tbl);

END
$$ LANGUAGE plpgsql SECURITY DEFINER;
/** @endcond */

/** @cond INTERNAL */
/* Trigger function for DELETE statement provenance tracking (PostgreSQL 14+).
 * Overrides the common version; documented via delete_statement_trigger in provsql.common.sql. */
CREATE OR REPLACE FUNCTION delete_statement_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  query_text TEXT;
  delete_token UUID;
  old_token UUID;
  new_token UUID;
  r RECORD;
  enable_trigger BOOL;
BEGIN
  enable_trigger := current_setting('provsql.update_provenance', true);
  IF enable_trigger = 'f' THEN
    RETURN NULL;
  END IF;
  delete_token := public.uuid_generate_v4();

  PERFORM create_gate(delete_token, 'update');

  SELECT query
  INTO query_text
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts, valid_time)
  VALUES (delete_token, query_text, 'DELETE', current_user, CURRENT_TIMESTAMP, tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)));

  PERFORM set_config('provsql.update_provenance', 'off', false);
  EXECUTE format('INSERT INTO %I.%I SELECT * FROM OLD_TABLE;', TG_TABLE_SCHEMA, TG_TABLE_NAME);
  PERFORM set_config('provsql.update_provenance', 'on', false);

  FOR r IN (SELECT * FROM OLD_TABLE) LOOP
    old_token := r.provsql;
    new_token := provenance_monus(old_token, delete_token);

    PERFORM set_config('provsql.update_provenance', 'off', false);
    EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2;', TG_TABLE_SCHEMA, TG_TABLE_NAME)
    USING new_token, old_token;
    PERFORM set_config('provsql.update_provenance', 'on', false);
  END LOOP;

  RETURN NULL;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;
/** @endcond */

/**
 * @brief Trigger function for INSERT statement provenance tracking
 *
 * Records the insertion in update_provenance and multiplies provenance
 * tokens of inserted rows with the insert token.
 */
CREATE OR REPLACE FUNCTION insert_statement_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  query_text TEXT;
  insert_token UUID;
  old_token UUID;
  new_token UUID;
  r RECORD;
  enable_trigger BOOL;
BEGIN
  enable_trigger := current_setting('provsql.update_provenance', true);
  IF enable_trigger = 'f' THEN
    RETURN NULL;
  END IF;

  insert_token := public.uuid_generate_v4();

  PERFORM create_gate(insert_token, 'update');

  SELECT query
  INTO query_text
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts, valid_time)
  VALUES (insert_token, query_text, 'INSERT', current_user, CURRENT_TIMESTAMP, tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)));

  FOR r IN (SELECT * FROM NEW_TABLE) LOOP
    old_token := r.provsql;
    new_token := provenance_times(old_token, insert_token);
    PERFORM set_config('provsql.update_provenance', 'off', false);
    EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2;', TG_TABLE_SCHEMA, TG_TABLE_NAME)
    USING new_token, old_token;
    PERFORM set_config('provsql.update_provenance', 'on', false);
  END LOOP;

  RETURN NULL;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;

/**
 * @brief Trigger function for UPDATE statement provenance tracking
 *
 * Records the update in update_provenance. Multiplies new-row tokens
 * with the update token and applies monus to old-row tokens.
 */
CREATE OR REPLACE FUNCTION update_statement_trigger()
  RETURNS TRIGGER AS
$$
DECLARE
  query_text TEXT;
  update_token UUID;
  old_token UUID;
  new_token UUID;
  r RECORD;
  enable_trigger BOOL;
BEGIN
  enable_trigger := current_setting('provsql.update_provenance', true);
  IF enable_trigger = 'f' THEN
    RETURN NULL;
  END IF;
  update_token := public.uuid_generate_v4();

  PERFORM create_gate(update_token, 'update');

  SELECT query
  INTO query_text
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts, valid_time)
  VALUES (update_token, query_text, 'UPDATE', current_user, CURRENT_TIMESTAMP, tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)));

  FOR r IN (SELECT * FROM NEW_TABLE) LOOP
    old_token := r.provsql;
    new_token := provenance_times(old_token, update_token);

    PERFORM set_config('provsql.update_provenance', 'off', false);
    EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2;', TG_TABLE_SCHEMA, TG_TABLE_NAME)
    USING new_token, old_token;
    PERFORM set_config('provsql.update_provenance', 'on', false);
  END LOOP;

  PERFORM set_config('provsql.update_provenance', 'off', false);
  EXECUTE format('INSERT INTO %I.%I SELECT * FROM OLD_TABLE;', TG_TABLE_SCHEMA, TG_TABLE_NAME);
  PERFORM set_config('provsql.update_provenance', 'on', false);

  FOR r IN (SELECT * FROM OLD_TABLE) LOOP
    old_token := r.provsql;
    new_token := provenance_monus(old_token, update_token);

    PERFORM set_config('provsql.update_provenance', 'off', false);
    EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2;', TG_TABLE_SCHEMA, TG_TABLE_NAME)
    USING new_token, old_token;
    PERFORM set_config('provsql.update_provenance', 'on', false);
  END LOOP;

  RETURN NULL;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp SECURITY DEFINER;


/** @} */

/** @defgroup temporal_db Temporal DB (PostgreSQL 14+)
 *  Functions for temporal database support. These use provenance
 *  evaluation over the multirange semiring to track temporal validity
 *  of tuples.
 *  @{
 */

/** @brief Transition function for temporal union (plus): merge multiranges */
SET search_path TO provsql;
CREATE OR REPLACE FUNCTION union_tstzintervals_plus_state(
    state tstzmultirange,
    value tstzmultirange
)
RETURNS tstzmultirange AS
$$
  SELECT CASE WHEN state IS NULL THEN value ELSE state + value END
$$ LANGUAGE SQL IMMUTABLE;

/** @brief Transition function for temporal intersection (times): intersect multiranges */
CREATE OR REPLACE FUNCTION union_tstzintervals_times_state(
    state tstzmultirange,
    value tstzmultirange
)
RETURNS tstzmultirange AS
$$
  SELECT CASE WHEN state IS NULL THEN value ELSE state * value END
$$ LANGUAGE SQL IMMUTABLE;

/** @brief Aggregate: union of timestamp multiranges (semiring plus) */
CREATE OR REPLACE AGGREGATE union_tstzintervals_plus(tstzmultirange)
(
  sfunc    = union_tstzintervals_plus_state,
  stype    = tstzmultirange,
  initcond = '{}'
);

/** @brief Aggregate: intersection of timestamp multiranges (semiring times) */
CREATE OR REPLACE AGGREGATE union_tstzintervals_times(tstzmultirange)
(
  sfunc    = union_tstzintervals_times_state,
  stype    = tstzmultirange,
  initcond = '{(,)}'
);

/** @brief Temporal monus: subtract one multirange from another */
CREATE OR REPLACE FUNCTION union_tstzintervals_monus(
    state tstzmultirange,
    value tstzmultirange
)
RETURNS tstzmultirange AS
$$
  SELECT CASE WHEN state <@ value THEN '{}'::tstzmultirange ELSE state - value END
$$ LANGUAGE SQL IMMUTABLE STRICT;

/**
 * @brief Evaluate temporal provenance as a timestamp multirange
 *
 * Evaluates provenance over the multirange semiring to compute the
 * valid time intervals of a tuple.
 *
 * @param token provenance token to evaluate
 * @param token2value mapping table from tokens to temporal validity ranges
 */
CREATE OR REPLACE FUNCTION union_tstzintervals(
    token UUID,
    token2value regclass
)
RETURNS tstzmultirange AS
$$
BEGIN
  RETURN provenance_evaluate(
    token,
    token2value,
    '{(,)}'::tstzmultirange,
    'union_tstzintervals_plus',
    'union_tstzintervals_times',
    'union_tstzintervals_monus'
  );
END
$$ LANGUAGE plpgsql PARALLEL SAFE;

/**
 * @brief Query a table as it was at a specific point in time
 *
 * Returns all rows whose temporal validity includes the given timestamp.
 *
 * @param tablename name of the provenance-tracked table
 * @param at_time the point in time to query
 */
CREATE OR REPLACE FUNCTION timetravel(
  tablename text,
  at_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$$
BEGIN
    RETURN QUERY EXECUTE format(
        '
          SELECT
            %1$I.*,
            union_tstzintervals(provenance(), ''%2$I'')
          FROM
            %1$I
          WHERE
            union_tstzintervals(provenance(), ''%2$I'') @> %3$L::timestamptz
        ',
        tablename,
        'time_validity_view',
        at_time::text
    );
END;
$$;

/**
 * @brief Query a table for rows valid during a time interval
 *
 * Returns all rows whose temporal validity overlaps the given range.
 *
 * @param tablename name of the provenance-tracked table
 * @param from_time start of the time interval
 * @param to_time end of the time interval
 */
CREATE OR REPLACE FUNCTION timeslice(
  tablename text,
  from_time timestamptz,
  to_time timestamptz
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$$
BEGIN
  RETURN QUERY EXECUTE format(
    '
      SELECT
        %1$I.*,
        union_tstzintervals(provenance(), ''%2$I'')
      FROM
        %1$I
      WHERE
        union_tstzintervals(provenance(), ''%2$I'')
        && tstzrange(%3$L::timestamptz, %4$L::timestamptz)
    ',
    tablename,
    'time_validity_view',
    from_time::text,
    to_time::text
  );
END;
$$;

/**
 * @brief Query the full temporal history of specific rows
 *
 * Returns all versions of rows matching the given column values,
 * with their temporal validity ranges.
 *
 * @param tablename name of the provenance-tracked table
 * @param col_names array of column names to filter on
 * @param col_values array of corresponding values to match
 */
CREATE OR REPLACE FUNCTION history(
  tablename text,
  col_names text[],
  col_values text[]
)
RETURNS SETOF record
LANGUAGE plpgsql
AS
$$
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
          union_tstzintervals(provenance(), ''%I'')
        FROM
          %I
        WHERE
          %s
      ',
      tablename,
      'time_validity_view',
      tablename,
      condition
    );
END;
$$;

/**
 * @brief Get the valid time range for a specific tuple
 *
 * @param token provenance token of the tuple
 * @param tablename name of the table containing the tuple
 */
CREATE OR REPLACE FUNCTION get_valid_time(
  token uuid,
  tablename text
)
RETURNS tstzmultirange
LANGUAGE plpgsql
AS $$
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
    'time_validity_view',
    tablename,
    token
  )
  INTO result;

  RETURN result;
END;
$$;

/**
 * @brief Undo a previously recorded update operation
 *
 * Traverses all provenance-tracked tables and rewrites their circuits
 * to apply monus with respect to the given update token, effectively
 * undoing the operation.
 *
 * @param c UUID of the update operation to undo (from update_provenance)
 */
CREATE OR REPLACE FUNCTION undo(
  c uuid
)
RETURNS uuid
LANGUAGE plpgsql
AS $$
DECLARE
  undo_query text;
  undone_query text;
  undo_token uuid;
  schema_rec RECORD;
  table_rec RECORD;
  row_rec RECORD;
  new_x uuid;
BEGIN
  SELECT query INTO undone_query
  FROM update_provenance
  WHERE provsql = c
  LIMIT 1;

  IF undone_query IS NULL THEN
    RAISE NOTICE 'Unable to find % in update_provenance', c;
    RETURN c;
  END IF;

  SELECT query
  INTO undo_query
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  undo_token := public.uuid_generate_v4();
  PERFORM create_gate(undo_token, 'update');
  INSERT INTO update_provenance(provsql, query, query_type, username, ts, valid_time)
  VALUES (
    undo_token,
    undo_query,
    'UNDO',
    current_user,
    CURRENT_TIMESTAMP,
    tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL))
  );

  PERFORM set_config('provsql.update_provenance', 'off', false);

  FOR schema_rec IN
    SELECT nspname
    FROM pg_namespace
    WHERE nspname NOT IN ('pg_catalog','information_schema','pg_toast','pg_temp_1','pg_toast_temp_1')
  LOOP
    FOR table_rec IN
      EXECUTE format('SELECT tablename AS tname FROM pg_tables WHERE schemaname = %L', schema_rec.nspname)
    LOOP
      IF EXISTS (
        SELECT 1
        FROM information_schema.columns
        WHERE table_schema = schema_rec.nspname
          AND table_name = table_rec.tname
          AND table_name <> 'update_provenance'
          AND column_name = 'provsql'
      ) THEN
        FOR row_rec IN
          EXECUTE format('SELECT provsql AS x FROM %I.%I', schema_rec.nspname, table_rec.tname)
        LOOP
          new_x := replace_the_circuit(row_rec.x, c, undo_token);
          EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2',
                         schema_rec.nspname, table_rec.tname)
          USING new_x, row_rec.x;
        END LOOP;
      END IF;
    END LOOP;
  END LOOP;

  PERFORM set_config('provsql.update_provenance', 'on', false);

  RETURN undo_token;
END;
$$;

/**
 * @brief Recursively rewrite a circuit to undo a specific operation
 *
 * Helper for undo(). Walks the circuit and replaces occurrences of
 * the target update gate with its monus.
 *
 * @param x provenance token to rewrite
 * @param c UUID of the update operation to undo
 * @param u UUID of the undo operation
 */
CREATE OR REPLACE FUNCTION replace_the_circuit(
  x uuid,
  c uuid,
  u uuid
)
RETURNS uuid
LANGUAGE plpgsql
AS $$
DECLARE
  nchildren uuid[];
  child uuid;
  ntoken uuid;
  ntype provenance_gate;
BEGIN
  IF x = c THEN
    RETURN provenance_monus(c, u);
  -- update and input gates cannot have children
  ELSIF get_gate_type(x) = 'update' OR get_gate_type(x) = 'input' THEN
    RETURN x;
  ELSE
    nchildren := '{}';
    FOREACH child IN ARRAY get_children(x)
    LOOP
      nchildren := array_append(nchildren, replace_the_circuit(child, c, u));
    END LOOP;

    ntoken := public.uuid_generate_v4();
    ntype := get_gate_type(x);

    PERFORM create_gate(ntoken, ntype, nchildren);
    RETURN ntoken;
  END IF;
END;
$$;

SELECT create_provenance_mapping_view('time_validity_view', 'update_provenance', 'valid_time');

/** @} */

SET search_path TO public;
