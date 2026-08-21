-- ----------------------------------------------------------------------
-- provsql 1.12.0 -> 1.13.0
--
-- This release makes the provenance circuit behave, under transactions,
-- the way the rest of the database does.  Three things move:
--
--   * Per-relation metadata (TID / BID / OPAQUE, block keys, ancestry)
--     leaves the fifth mmap file for the provsql.table_info heap table,
--     so it rolls back with the transaction that wrote it and pg_dump
--     carries it.  migrate_table_info() imports whatever the old file
--     still holds; it is a no-op on a database that never had one.
--
--   * A probability is written once: set_prob writes one on a gate that
--     has none, accepts the identical value again, and refuses a
--     different one, and a write a transaction rolls back is cleared.
--     replace_input / replace_block / replace_update are how one
--     changes; provenance_guard recognises the leaf they mint and keeps
--     the relation's kind, and provenance_mapping_registry gains a
--     `maintained` flag so that snapshot mappings are registered too and
--     follow a token to its replacement.
--
--   * repair_key records a block's size in info2 rather than writing the
--     uniform 1/size as a probability, so the documented "repair_key
--     then set_prob(provenance(), p)" is still a first write.
--
-- Plus the store-maintenance surface -- check_store() and
-- circuit_cleanup() -- and, on PostgreSQL 14+, transaction-level data
-- modification: one update gate per transaction, update_provenance.xid
-- and .tx_token, undo() at either granularity, and commit-time validity.
--
-- Existing rows of provenance_mapping_registry are back-filled with
-- maintained = true: before this release, a row was only inserted for a
-- mapping created with maintained => true.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- ----------------------------------------------------------------------
-- 1. The two kinds the data-modification log now records.
-- ----------------------------------------------------------------------

ALTER TYPE query_type_enum ADD VALUE IF NOT EXISTS 'TRANSACTION' AFTER 'UNDO';
ALTER TYPE query_type_enum ADD VALUE IF NOT EXISTS 'REPLACE' AFTER 'TRANSACTION';

-- ----------------------------------------------------------------------
-- 2. Per-relation metadata in the heap.
--
--    relid is a regclass so that a dump carries the relation's name:
--    OIDs are not stable across databases.  The row trigger issues the
--    relcache invalidation that drops the stale entry from every
--    backend's cache, which covers the setter functions, a hand-written
--    UPDATE on the table, and the COPY a pg_restore performs.
-- ----------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS table_info(
  relid     regclass PRIMARY KEY,
  kind      text     NOT NULL,
  block_key int2[]   NOT NULL DEFAULT ARRAY[]::int2[],
  ancestors oid[]    NOT NULL DEFAULT ARRAY[]::oid[]
);
SELECT pg_catalog.pg_extension_config_dump('table_info', '');

CREATE OR REPLACE FUNCTION table_info_invalidate()
  RETURNS trigger AS
  'provsql','provsql_table_info_invalidate' LANGUAGE C;

DO $do$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_trigger
                  WHERE tgrelid = 'provsql.table_info'::regclass
                    AND tgname = 'table_info_invalidate') THEN
    CREATE TRIGGER table_info_invalidate
      AFTER INSERT OR UPDATE OR DELETE ON provsql.table_info
      FOR EACH ROW EXECUTE PROCEDURE provsql.table_info_invalidate();
  END IF;
END $do$;

CREATE OR REPLACE FUNCTION set_table_info(
  relid OID, kind TEXT, block_key INT2[] DEFAULT ARRAY[]::INT2[])
  RETURNS void AS
  'provsql','set_table_info' LANGUAGE C SECURITY DEFINER;

CREATE OR REPLACE FUNCTION remove_table_info(relid OID)
  RETURNS void AS
  'provsql','remove_table_info' LANGUAGE C SECURITY DEFINER;

CREATE OR REPLACE FUNCTION set_ancestors(
  relid OID, ancestors OID[] DEFAULT ARRAY[]::OID[])
  RETURNS void AS
  'provsql','set_ancestors' LANGUAGE C SECURITY DEFINER;

CREATE OR REPLACE FUNCTION remove_ancestors(relid OID)
  RETURNS void AS
  'provsql','remove_ancestors' LANGUAGE C SECURITY DEFINER;

CREATE OR REPLACE FUNCTION migrate_table_info()
  RETURNS BIGINT AS
  'provsql','migrate_table_info' LANGUAGE C SECURITY DEFINER;

-- Import whatever provsql_table_info.mmap still holds.  Zero rows on a
-- database that never had the file, or that has already been migrated.
SELECT migrate_table_info();

-- ----------------------------------------------------------------------
-- 3. Every provenance mapping is registered, not only the maintained
--    ones: a snapshot mapping must follow a row's token when
--    replace_input gives that row a new one.
-- ----------------------------------------------------------------------

DO $do$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_attribute
                  WHERE attrelid = 'provsql.provenance_mapping_registry'::regclass
                    AND attname = 'maintained' AND NOT attisdropped) THEN
    ALTER TABLE provsql.provenance_mapping_registry
      ADD COLUMN maintained boolean NOT NULL DEFAULT false;
    -- Every row that existed before this release was a maintained one.
    UPDATE provsql.provenance_mapping_registry SET maintained = true;
  END IF;
END $do$;

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
  -- The mapping is keyed by gate identity (input-token UUIDs), so peel any
  -- transparent annotation wrapper (e.g. the inversion-free certificate a
  -- certified query attaches to its row roots) off the captured tokens.
  UPDATE tmp_provsql SET provenance = provsql.strip_annotations(provenance)
    WHERE provsql.get_gate_type(provenance) = 'annotation';
  IF preserve_case THEN
    EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
  ELSE
    EXECUTE format('CREATE TABLE %s AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %s(provenance)', newtbl);
  END IF;
  -- Register the mapping.  When maintained, genuine inserts into oldtbl
  -- keep it current (see provenance_guard); keyed to the input token, so
  -- it survives the provsql rewrites that data modification performs.
  -- A snapshot mapping is registered too, so that replacing a row's input
  -- gate (provsql.replace_input) carries the row's value over to the new
  -- token: the tuple is the same one, only its token moved.
  INSERT INTO provsql.provenance_mapping_registry(mapping, source, attribute, maintained)
    VALUES (
      (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
            ELSE to_regclass(newtbl) END)::oid,
      oldtbl::oid, att, maintained)
    ON CONFLICT (mapping)
      DO UPDATE SET source = EXCLUDED.source, attribute = EXCLUDED.attribute,
                    maintained = EXCLUDED.maintained;
END
$$ LANGUAGE plpgsql;

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
                  FROM provsql.provenance_mapping_registry
                 WHERE source = TG_RELID AND maintained
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
      IF provsql.is_fresh_leaf(NEW.provsql) THEN
        -- A replacement leaf minted by provsql.replace_input /
        -- replace_block in this transaction: an independent fresh leaf by
        -- construction, so the table's kind survives.  Carry the
        -- maintained mappings over from the token it replaces, the same
        -- job the INSERT branch does for a new row.
        FOR _m IN SELECT mapping, attribute
                    FROM provsql.provenance_mapping_registry WHERE source = TG_RELID
        LOOP
          EXECUTE format(
            'INSERT INTO %1$s(value, provenance) '
            'SELECT value, $2 FROM %1$s WHERE provenance = $1',
            _m.mapping::regclass)
            USING OLD.provsql, NEW.provsql;
        END LOOP;
      ELSE
        PERFORM provsql.set_table_info(TG_RELID, 'opaque');
      END IF;
    END IF;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER;

-- ----------------------------------------------------------------------
-- 4. Write-once probabilities, and how one changes.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION set_prob(
  token UUID, p DOUBLE PRECISION)
  RETURNS void AS
  'provsql','set_prob' LANGUAGE C PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION probability_is_set(token UUID)
  RETURNS BOOLEAN AS
  'provsql','probability_is_set' LANGUAGE C STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION note_fresh_leaf(token UUID)
  RETURNS void AS
  'provsql','note_fresh_leaf' LANGUAGE C;

CREATE OR REPLACE FUNCTION is_fresh_leaf(token UUID)
  RETURNS BOOLEAN AS
  'provsql','is_fresh_leaf' LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION replace_input(old UUID, p DOUBLE PRECISION)
  RETURNS UUID AS
$$
DECLARE
  t UUID;
  tp provsql.provenance_gate;
BEGIN
  IF old IS NULL OR p IS NULL THEN
    RAISE EXCEPTION 'replace_input: neither argument may be NULL';
  END IF;
  tp := provsql.get_gate_type(old);
  IF tp = 'mulinput' THEN
    RAISE EXCEPTION 'replace_input: % belongs to a repair_key block', old
      USING HINT = 'A block''s values share one key gate and their masses '
                   'are meaningful together, so they are replaced together: '
                   'use provsql.replace_block().';
  ELSIF tp = 'update' THEN
    RAISE EXCEPTION 'replace_input: % is an update gate', old
      USING HINT = 'Use provsql.replace_update() to give a recorded data '
                   'modification a different probability.';
  ELSIF tp <> 'input' THEN
    RAISE EXCEPTION 'replace_input: % is a gate of type %, not an input', old, tp
      USING HINT = 'Only a leaf carries a probability of its own; a derived '
                   'gate''s is computed from its leaves.';
  END IF;
  t := public.uuid_generate_v4();
  PERFORM provsql.create_gate(t, 'input');
  PERFORM provsql.set_prob(t, p);
  PERFORM provsql.note_fresh_leaf(t);
  RETURN t;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION replace_input(
  _tbl regclass, old UUID, p DOUBLE PRECISION)
  RETURNS UUID AS
$$
DECLARE
  t UUID;
  n INT;
BEGIN
  t := provsql.replace_input(old, p);
  EXECUTE format('UPDATE %s SET provsql = $1 WHERE provsql = $2', _tbl)
    USING t, old;
  GET DIAGNOSTICS n = ROW_COUNT;
  IF n = 0 THEN
    RAISE EXCEPTION 'replace_input: no row of % carries the token %', _tbl, old;
  END IF;
  RETURN t;
END
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION replace_block(
  _tbl regclass, old_key UUID, probs DOUBLE PRECISION[] DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  r RECORD;
  n INT;
  i INT := 0;
  new_key UUID;
  new_tok UUID;
  was_active TEXT;
BEGIN
  IF provsql.get_gate_type(old_key) <> 'input' THEN
    RAISE EXCEPTION 'replace_block: % is not a block key gate', old_key;
  END IF;

  -- The rewriter has no business in the bookkeeping below: the tokens of
  -- _tbl are what this function is here to rewrite, not provenance to
  -- carry into a temporary table.  Restored before returning; a failure
  -- aborts the transaction, which restores it too.
  was_active := coalesce(current_setting('provsql.active', true), 'on');
  PERFORM set_config('provsql.active', 'off', true);

  EXECUTE format(
    'CREATE TEMP TABLE provsql_replace_block_tmp ON COMMIT DROP AS
       SELECT t.provsql AS old_token,
              NULL::uuid AS new_token,
              (provsql.get_infos(t.provsql)).info1 AS ord
         FROM %s t
        WHERE provsql.get_gate_type(t.provsql) = ''mulinput''
          AND (provsql.get_children(t.provsql))[1] = %L', _tbl, old_key);

  SELECT count(*) INTO n FROM provsql_replace_block_tmp;
  IF n = 0 THEN
    RAISE EXCEPTION 'replace_block: no row of % belongs to block %', _tbl, old_key;
  END IF;
  IF probs IS NOT NULL AND array_length(probs, 1) <> n THEN
    RAISE EXCEPTION 'replace_block: block % has % rows but % probabilities were given',
      old_key, n, array_length(probs, 1);
  END IF;

  new_key := public.uuid_generate_v4();
  PERFORM provsql.create_gate(new_key, 'input');

  FOR r IN SELECT old_token, ord FROM provsql_replace_block_tmp ORDER BY ord LOOP
    i := i + 1;
    new_tok := public.uuid_generate_v4();
    PERFORM provsql.create_gate(new_tok, 'mulinput', ARRAY[new_key]);
    PERFORM provsql.set_infos(new_tok, r.ord, n);
    IF probs IS NOT NULL THEN
      PERFORM provsql.set_prob(new_tok, probs[i]);
    END IF;
    PERFORM provsql.note_fresh_leaf(new_tok);
    UPDATE provsql_replace_block_tmp SET new_token = new_tok
      WHERE old_token = r.old_token;
  END LOOP;

  EXECUTE format(
    'UPDATE %s t SET provsql = b.new_token
       FROM provsql_replace_block_tmp b WHERE t.provsql = b.old_token', _tbl);

  DROP TABLE provsql_replace_block_tmp;
  PERFORM set_config('provsql.active', was_active, true);
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
  -- Resolve the (possibly comma-separated) key_att text into the
  -- corresponding pg_attribute.attnum values for the safe-query
  -- metadata.  Names are trimmed; quoting is not supported because
  -- repair_key has never accepted quoted identifiers in key_att.
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

  -- Same column shape as add_provenance: no UNIQUE, no DEFAULT past
  -- the initial backfill (the guard trigger added after the rename
  -- takes over both jobs once the column has been renamed to its
  -- final name).  The DEFAULT is kept here only so the second pass
  -- below can read provsql_temp from the user-visible rows
  -- without a separate UPDATE.
  EXECUTE format('ALTER TABLE %s ADD COLUMN provsql_temp UUID DEFAULT public.uuid_generate_v4()', _tbl);

  -- Build a per-group mapping (key columns + a fresh key_token + the
  -- group size) once, then use it for both the create_gate(key_token,
  -- 'input') first pass and the per-row mulinput second pass.  Going
  -- through a temp table avoids re-running uuid_generate_v4() (which
  -- would produce different UUIDs the second time).  USING (%1$s) on
  -- the second pass handles the multi-column case uniformly.
  -- ON COMMIT DROP plus the explicit DROP TABLE at the end of this
  -- function leave the temp table cleaned up across transactions and
  -- across repeated calls in the same transaction.
  IF key_att = '' THEN
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %s', _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %s t CROSS JOIN provsql_repair_key_tmp k', _tbl);
  ELSE
    EXECUTE format(
      'CREATE TEMP TABLE provsql_repair_key_tmp ON COMMIT DROP AS
         SELECT %1$s,
                public.uuid_generate_v4() AS provsql_key_token,
                COUNT(*) AS provsql_group_size
           FROM %2$s
       GROUP BY %1$s', key_att, _tbl);
    rows_query := format(
      'SELECT t.provsql_temp,
              k.provsql_key_token AS key_token,
              ROW_NUMBER() OVER (PARTITION BY k.provsql_key_token
                                 ORDER BY t.ctid) AS within_group,
              k.provsql_group_size AS group_size
         FROM %2$s t
         JOIN provsql_repair_key_tmp k USING (%1$s)', key_att, _tbl);
  END IF;

  -- Pass 1: one input gate per group key.
  FOR r IN SELECT provsql_key_token FROM provsql_repair_key_tmp LOOP
    PERFORM provsql.create_gate(r.provsql_key_token, 'input');
  END LOOP;

  -- Pass 2: per row, attach a mulinput gate to its group's key token.
  -- The block size goes in info2 rather than the uniform 1/size going
  -- in the probability: a repaired row's probability is the user's to
  -- write (the documented "repair_key then set_prob(provenance(), p)"
  -- pattern), and probabilities are written once.  A row nobody gives
  -- a probability evaluates at 1/size all the same -- see
  -- MMappedCircuit::getProb.
  FOR r IN EXECUTE rows_query LOOP
    PERFORM provsql.create_gate(r.provsql_temp, 'mulinput', ARRAY[r.key_token]);
    PERFORM provsql.set_infos(r.provsql_temp, r.within_group::int,
                              r.group_size::int);
  END LOOP;

  DROP TABLE provsql_repair_key_tmp;

  EXECUTE format('ALTER TABLE %s ALTER COLUMN provsql_temp DROP DEFAULT', _tbl);
  EXECUTE format('ALTER TABLE %s RENAME COLUMN provsql_temp TO provsql', _tbl);
  EXECUTE format('CREATE INDEX ON %s(provsql)', _tbl);
  EXECUTE format(
    'CREATE TRIGGER provenance_guard BEFORE INSERT OR UPDATE OF provsql '
    'ON %s FOR EACH ROW EXECUTE PROCEDURE provsql.provenance_guard()',
    _tbl);
  PERFORM provsql.set_table_info(_tbl::oid, 'bid', block_key_cols);
  -- Base BID tables also have themselves as their sole ancestor.  Same
  -- rationale as the @c add_provenance branch above.
  PERFORM provsql.set_ancestors(_tbl::oid, ARRAY[_tbl::oid]);
END
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- 5. Store maintenance: what does not add up, and reclaiming what
--    nothing references any more.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION check_store(
  OUT unclean_shutdown BOOLEAN,
  OUT nb_gates BIGINT,
  OUT nb_tokens BIGINT,
  OUT next_index BIGINT,
  OUT dangling_indices BIGINT,
  OUT unreferenced BIGINT,
  OUT bad_wires BIGINT,
  OUT bad_extra BIGINT)
  RETURNS record AS
  'provsql', 'check_store' LANGUAGE C;

CREATE OR REPLACE FUNCTION circuit_cleanup(
  dry_run BOOLEAN DEFAULT false,
  OUT gates_before BIGINT,
  OUT gates_after BIGINT,
  OUT wires_before BIGINT,
  OUT wires_after BIGINT,
  OUT extra_bytes_before BIGINT,
  OUT extra_bytes_after BIGINT)
  RETURNS record AS
  'provsql', 'circuit_cleanup' LANGUAGE C;

-- ----------------------------------------------------------------------
-- 6. PostgreSQL 14+: the transaction as a unit of data modification.
--
--    The statement triggers, undo() and update_provenance only exist
--    where sql/provsql.14.sql was loaded, so everything here is guarded
--    on the server version rather than assumed.
-- ----------------------------------------------------------------------

DO $do$ BEGIN
  IF current_setting('server_version_num')::int < 140000 THEN
    RETURN;
  END IF;

  EXECUTE $sql$
ALTER TABLE provsql.update_provenance ADD COLUMN IF NOT EXISTS xid xid8;
  $sql$;
  EXECUTE $sql$
ALTER TABLE provsql.update_provenance ADD COLUMN IF NOT EXISTS tx_token uuid;
  $sql$;

  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;

  IF NOT EXISTS (SELECT 1 FROM pg_trigger
                  WHERE tgrelid = 'provsql.update_provenance'::regclass
                    AND tgname = 'stamp_commit_time') THEN
    EXECUTE $sql$
CREATE CONSTRAINT TRIGGER stamp_commit_time
  AFTER INSERT ON provsql.update_provenance
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE provsql.stamp_commit_time();
    $sql$;
  END IF;

  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;
  EXECUTE $sql$
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
$sql$;
END $do$;

-- ----------------------------------------------------------------------
-- 7. The C side caches the OID of each enum value per session; a backend
--    warmed under the previous version would not know the two values
--    added in section 1.
-- ----------------------------------------------------------------------

SELECT reset_constants_cache();
