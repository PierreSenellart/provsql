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
 *
 * A row of type @c TRANSACTION stands for the transaction the statements
 * around it belong to (see @c provsql.transaction_token): @c xid is its
 * transaction id and its own @c tx_token is NULL, while every statement
 * row of that transaction carries the transaction's token in @c tx_token.
 * A modified tuple's provenance names both -- the effect is
 * @c times(tx_token, statement_token) -- so @c undo() reverses either one
 * statement or the whole transaction, and the temporal semiring reads the
 * transaction's validity through the shared factor.
 *
 * @c ts and the lower bound of @c valid_time are stamped at commit, not
 * at the statement: @c CURRENT_TIMESTAMP is the transaction's start time,
 * so two overlapping transactions could otherwise commit in the opposite
 * order of their recorded validity.
 */
CREATE TABLE update_provenance (
  provsql uuid,
  query text,
  query_type query_type_enum,
  username text,
  ts timestamp DEFAULT CURRENT_TIMESTAMP,
  valid_time tstzmultirange DEFAULT tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
  xid xid8,
  tx_token uuid
);
/**
 * @brief The update gate standing for the current transaction
 *
 * Each data-modification statement already mints an @c update gate of its
 * own, but nothing tied the statements of one transaction together: a
 * reader of @c update_provenance could not tell that two rows came from
 * the same transaction, and @c undo() could reverse a statement but not
 * "the transaction".  This mints one gate per transaction, on the first
 * tracked modification, and hands the same one back for the rest of it.
 *
 * Where a transaction's gate lives is @c SET @c LOCAL, so it vanishes
 * when the transaction ends, whether it commits or rolls back -- and a
 * rolled-back transaction leaves no @c update_provenance row for it
 * either, since that row is an ordinary heap insert.
 */
CREATE OR REPLACE FUNCTION transaction_token()
RETURNS uuid
LANGUAGE plpgsql
AS $$
DECLARE
  tok text;
  new_tok uuid;
  query_text text;
BEGIN
  tok := current_setting('provsql.transaction_token', true);
  IF tok IS NOT NULL AND tok <> '' THEN
    RETURN tok::uuid;
  END IF;

  new_tok := public.uuid_generate_v4();
  PERFORM create_gate(new_tok, 'update');
  PERFORM set_config('provsql.transaction_token', new_tok::text, true);

  -- A transaction has no query text of its own: query is left NULL, so
  -- looking a statement up by its text finds the statement and not the
  -- transaction that carried it.
  query_text := NULL;

  -- The transaction's own validity is the universal range, the
  -- multiplicative identity of the temporal m-semiring: it is a factor of
  -- every effect of the transaction, and what a tuple is valid for is the
  -- statement's business, not the transaction's.  Giving it a real
  -- interval would intersect it into every one of them.
  INSERT INTO update_provenance(provsql, query, query_type, username, ts,
                                valid_time, xid)
  VALUES (new_tok, query_text, 'TRANSACTION', current_user,
          CURRENT_TIMESTAMP, '{(,)}'::tstzmultirange,
          pg_current_xact_id());

  RETURN new_tok;
END;
$$;

/**
 * @brief Deferred trigger stamping a log row with its commit time
 *
 * @c CURRENT_TIMESTAMP is the transaction's *start* time, so two
 * overlapping transactions can commit in the opposite order of the
 * validity they recorded.  This fires at commit -- it is a constraint
 * trigger declared @c DEFERRABLE @c INITIALLY @c DEFERRED -- and moves
 * the row's timestamp and the lower bound of its validity to
 * @c clock_timestamp(), which by then is the commit time to within the
 * commit itself.
 */
CREATE OR REPLACE FUNCTION stamp_commit_time()
  RETURNS trigger AS
$$
DECLARE
  now_ts timestamptz := clock_timestamp();
BEGIN
  UPDATE update_provenance
     SET ts = now_ts,
         valid_time = CASE WHEN query_type = 'TRANSACTION' THEN valid_time
                           ELSE tstzmultirange(tstzrange(now_ts, NULL)) END
   WHERE provsql = NEW.provsql;
  RETURN NULL;
END;
$$ LANGUAGE plpgsql;

DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_trigger
                  WHERE tgrelid = 'provsql.update_provenance'::regclass
                    AND tgname = 'stamp_commit_time') THEN
    CREATE CONSTRAINT TRIGGER stamp_commit_time
      AFTER INSERT ON provsql.update_provenance
      DEFERRABLE INITIALLY DEFERRED
      FOR EACH ROW EXECUTE PROCEDURE provsql.stamp_commit_time();
  END IF;
END $$;

/** @cond INTERNAL */
/* Enable provenance tracking on an existing table (PostgreSQL 14+ version).
 * Overrides the common version; documented via add_provenance in provsql.common.sql. */
CREATE OR REPLACE FUNCTION add_provenance(_tbl regclass)
  RETURNS void AS
$$
BEGIN
  -- Idempotence: a second add_provenance on an already-tracked table is
  -- a no-op with a NOTICE, so setup scripts and notebook cells can be
  -- re-run freely.
  IF EXISTS (
    SELECT 1 FROM pg_attribute
    WHERE attrelid = _tbl AND attname = 'provsql' AND NOT attisdropped
  ) THEN
    RAISE NOTICE 'table % already has provenance tracking', _tbl;
    RETURN;
  END IF;
  -- See the common-version body for the rationale of dropping the
  -- column DEFAULT and UNIQUE in favour of provenance_guard + a
  -- plain index.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql UUID', _tbl);
  EXECUTE format(
    'UPDATE %s SET provsql = public.uuid_generate_v4() WHERE provsql IS NULL',
    _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);

  EXECUTE format('CREATE TRIGGER insert_statement AFTER INSERT ON %s REFERENCING NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.insert_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER delete_statement AFTER DELETE ON %s REFERENCING OLD TABLE AS OLD_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.delete_statement_trigger()', _tbl);
  EXECUTE format('CREATE TRIGGER update_statement AFTER UPDATE ON %s REFERENCING OLD TABLE AS OLD_TABLE NEW TABLE AS NEW_TABLE FOR EACH STATEMENT EXECUTE PROCEDURE provsql.update_statement_trigger()', _tbl);

  PERFORM provsql.set_table_info(_tbl::oid, 'tid');
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
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
  tx_token UUID;
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

  tx_token := transaction_token();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts,
                                 valid_time, xid, tx_token)
  VALUES (delete_token, query_text, 'DELETE', current_user, CURRENT_TIMESTAMP,
          tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
          pg_current_xact_id(), tx_token);

  -- The effect this statement has on a row names both the statement and
  -- the transaction it belongs to, so undo() can reverse either.
  delete_token := provenance_times(tx_token, delete_token);

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
  tx_token UUID;
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

  tx_token := transaction_token();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts,
                                 valid_time, xid, tx_token)
  VALUES (insert_token, query_text, 'INSERT', current_user, CURRENT_TIMESTAMP,
          tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
          pg_current_xact_id(), tx_token);

  -- The effect this statement has on a row names both the statement and
  -- the transaction it belongs to, so undo() can reverse either.
  insert_token := provenance_times(tx_token, insert_token);

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
  tx_token UUID;
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

  tx_token := transaction_token();

  INSERT INTO update_provenance (provsql, query, query_type, username, ts,
                                 valid_time, xid, tx_token)
  VALUES (update_token, query_text, 'UPDATE', current_user, CURRENT_TIMESTAMP,
          tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
          pg_current_xact_id(), tx_token);

  -- The effect this statement has on a row names both the statement and
  -- the transaction it belongs to, so undo() can reverse either.
  update_token := provenance_times(tx_token, update_token);

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

SET search_path TO provsql;

/**
 * @brief Evaluate provenance over the temporal (interval-union) m-semiring
 *
 * Inputs are read as %tstzmultirange validity intervals; the additive
 * identity is <tt>'{}'::%tstzmultirange</tt> (empty), the multiplicative
 * identity is <tt>'{(,)}'::%tstzmultirange</tt> (universal). Returns the union
 * of intervals supporting the result, computed via the compiled circuit
 * traversal.
 *
 * @param token       Provenance token to evaluate.
 * @param token2value Mapping from input gates to validity multiranges.
 */
CREATE FUNCTION sr_temporal(token ANYELEMENT, token2value regclass)
  RETURNS tstzmultirange AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::tstzmultirange
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/**
 * @brief Evaluate provenance over the interval-union m-semiring
 *        with a numeric multirange carrier
 *
 * Inputs are read as %nummultirange validity ranges over a numeric
 * domain (e.g. sensor measurement-validity ranges). Addition is
 * multirange union, multiplication is intersection, monus is set
 * difference; the additive identity is <tt>'{}'::%nummultirange</tt>
 * and the multiplicative identity is <tt>'{(,)}'::%nummultirange</tt>
 * (universal range).
 *
 * @param token       Provenance token to evaluate.
 * @param token2value Mapping from input gates to numeric multiranges.
 */
CREATE FUNCTION sr_interval_num(token ANYELEMENT, token2value regclass)
  RETURNS nummultirange AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::nummultirange
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/**
 * @brief Evaluate provenance over the interval-union m-semiring
 *        with an int4 multirange carrier
 *
 * Inputs are read as %int4multirange validity ranges over the
 * integers (e.g. page or line ranges of supporting documents).
 * Addition is multirange union, multiplication is intersection,
 * monus is set difference; the additive identity is
 * <tt>'{}'::%int4multirange</tt> and the multiplicative identity is
 * <tt>'{(,)}'::%int4multirange</tt>.
 *
 * @param token       Provenance token to evaluate.
 * @param token2value Mapping from input gates to int4 multiranges.
 */
CREATE FUNCTION sr_interval_int(token ANYELEMENT, token2value regclass)
  RETURNS int4multirange AS
$$
BEGIN
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'interval_union',
    '{(,)}'::int4multirange
  );
END
$$ LANGUAGE plpgsql STRICT PARALLEL SAFE STABLE;

/**
 * @brief Evaluate temporal provenance as a timestamp multirange
 *
 * Thin wrapper around :sqlfunc:`sr_temporal` retained for backward
 * compatibility; both compute the same union of validity intervals.
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
  SELECT sr_temporal(token, token2value)
$$ LANGUAGE SQL PARALLEL SAFE STABLE;

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
            sr_temporal(provenance(), %2$L)
          FROM
            %1$I
          WHERE
            sr_temporal(provenance(), %2$L) @> %3$L::timestamptz
        ',
        tablename,
        'provsql.time_validity_view',
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
        sr_temporal(provenance(), %2$L)
      FROM
        %1$I
      WHERE
        sr_temporal(provenance(), %2$L)
        && tstzrange(%3$L::timestamptz, %4$L::timestamptz)
    ',
    tablename,
    'provsql.time_validity_view',
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
          sr_temporal(provenance(), %L)
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
        sr_temporal(provenance(), %L)
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
  -- Test for the row, not for its query text: a TRANSACTION row has no
  -- query of its own, and undoing a whole transaction is exactly what it
  -- is there for.
  SELECT query INTO undone_query
  FROM update_provenance
  WHERE provsql = c
  LIMIT 1;

  IF NOT FOUND THEN
    RAISE NOTICE 'Unable to find % in update_provenance', c;
    RETURN c;
  END IF;

  SELECT query
  INTO undo_query
  FROM pg_stat_activity
  WHERE pid = pg_backend_pid();

  undo_token := public.uuid_generate_v4();
  PERFORM create_gate(undo_token, 'update');
  INSERT INTO update_provenance(provsql, query, query_type, username, ts,
                                valid_time, xid, tx_token)
  VALUES (
    undo_token,
    undo_query,
    'UNDO',
    current_user,
    CURRENT_TIMESTAMP,
    tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
    pg_current_xact_id(),
    transaction_token()
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

/**
 * @brief Rewrite a circuit, substituting one gate for another
 *
 * Walks @p x and rebuilds every gate above an occurrence of @p old over
 * @p new instead.  Leaves that are not @p old come back unchanged, so a
 * token that does not mention @p old is returned as it is.
 *
 * @param x   the token to rewrite
 * @param old the gate to substitute away
 * @param new the gate to put in its place
 */
CREATE OR REPLACE FUNCTION substitute_gate(
  x uuid,
  old uuid,
  new uuid
)
RETURNS uuid
LANGUAGE plpgsql
AS $$
DECLARE
  nchildren uuid[];
  child uuid;
  rewritten uuid;
  changed boolean := false;
  ntoken uuid;
  ntype provenance_gate;
BEGIN
  IF x = old THEN
    RETURN new;
  END IF;
  ntype := get_gate_type(x);
  -- Leaves have no children to walk into.
  IF ntype IN ('input', 'update', 'rv', 'value', 'zero', 'one') THEN
    RETURN x;
  END IF;
  nchildren := '{}';
  FOREACH child IN ARRAY get_children(x)
  LOOP
    rewritten := substitute_gate(child, old, new);
    IF rewritten <> child THEN
      changed := true;
    END IF;
    nchildren := array_append(nchildren, rewritten);
  END LOOP;
  IF NOT changed THEN
    RETURN x;
  END IF;
  ntoken := public.uuid_generate_v4();
  PERFORM create_gate(ntoken, ntype, nchildren);
  RETURN ntoken;
END;
$$;

/**
 * @brief Give a recorded data modification a different probability
 *
 * The @c update-gate counterpart of @c provsql.replace_input.  A gate's
 * probability is written once, so "how likely is it that this
 * modification happened" is changed by minting a new @c update gate with
 * the new probability, logging it in @c update_provenance beside the one
 * it replaces, and rewriting every tracked row whose provenance mentions
 * the old gate to mention the new one instead -- the same walk @c undo
 * performs.  The old gate and its log row are kept: the history of the
 * database is not rewritten, it is extended.
 *
 * @param old the @c update gate to replace, as found in
 *            @c update_provenance
 * @param p   the new probability, in [0,1]
 * @return    the new @c update gate
 */
CREATE OR REPLACE FUNCTION replace_update(
  old uuid,
  p double precision
)
RETURNS uuid
LANGUAGE plpgsql
AS $$
DECLARE
  new_token uuid;
  old_row RECORD;
  schema_rec RECORD;
  table_rec RECORD;
  row_rec RECORD;
  new_x uuid;
BEGIN
  IF old IS NULL OR p IS NULL THEN
    RAISE EXCEPTION 'replace_update: neither argument may be NULL';
  END IF;
  IF get_gate_type(old) <> 'update' THEN
    RAISE EXCEPTION 'replace_update: % is not an update gate', old
      USING HINT = 'Use provsql.replace_input() for a tuple''s own input gate.';
  END IF;

  SELECT * INTO old_row FROM update_provenance WHERE provsql = old LIMIT 1;
  IF old_row IS NULL THEN
    RAISE EXCEPTION 'replace_update: % is not recorded in update_provenance', old;
  END IF;

  new_token := public.uuid_generate_v4();
  PERFORM create_gate(new_token, 'update');
  PERFORM set_prob(new_token, p);

  INSERT INTO update_provenance(provsql, query, query_type, username, ts,
                                valid_time, xid, tx_token)
  VALUES (new_token, old_row.query, 'REPLACE', current_user,
          CURRENT_TIMESTAMP,
          tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL)),
          pg_current_xact_id(), transaction_token());

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
          new_x := substitute_gate(row_rec.x, old, new_token);
          IF new_x <> row_rec.x THEN
            EXECUTE format('UPDATE %I.%I SET provsql = $1 WHERE provsql = $2',
                           schema_rec.nspname, table_rec.tname)
            USING new_x, row_rec.x;
          END IF;
        END LOOP;
      END IF;
    END LOOP;
  END LOOP;

  PERFORM set_config('provsql.update_provenance', 'on', false);

  RETURN new_token;
END;
$$;

-- The base validity mapping is a plain view over the data-modification log:
-- update_provenance is append-only and never has its provsql rewritten, so a
-- view stays correct (unlike a tracked table's mapping, which must be a
-- maintained mapping table -- see create_provenance_mapping(maintained)).
CREATE VIEW provsql.time_validity_view AS
  SELECT valid_time AS value, provsql AS provenance FROM provsql.update_provenance;

/** @} */

SET search_path TO public;

-- Final constants-cache refresh: same rationale as at the end of
-- provsql.common.sql.  On PG14+ this file is appended after the common
-- script, so this is the last statement of the generated install script;
-- the refresh must come after every object has been created for the
-- installing session's memoized constants to be complete.
SELECT provsql.reset_constants_cache();
