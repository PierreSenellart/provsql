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
-- 6b. Aggregates that see their NULL inputs (array_agg, json_agg, ...)
--     keep a NULL-valued row as a child of their gate.
-- ----------------------------------------------------------------------

/**
 * @brief Return the UUID of the value gate standing for the NULL value
 *
 * A constant, like gate_zero() and gate_one(); the gate itself is a
 * <tt>value</tt> gate that displays as <tt>NULL</tt>. Its UUID is what
 * tells it apart from the value gate of the string <tt>'NULL'</tt>; the
 * seed <tt>'null'</tt> is no <tt>'value' || text</tt>, so no actual value
 * shares it.
 */
CREATE OR REPLACE FUNCTION gate_null() RETURNS uuid AS
$$
  SELECT public.uuid_generate_v5(provsql.uuid_ns_provsql(),'null');
$$ LANGUAGE SQL IMMUTABLE PARALLEL SAFE;

/**
 * @brief Semimodule gate for an aggregate that sees its NULL inputs
 *
 * Variant of provenance_semimod() used by the query rewriter for
 * <tt>array_agg</tt>, <tt>json_agg</tt> and the like, whose result lists
 * every input, NULLs included: a NULL value still yields a semimod gate,
 * over the constant value gate gate_null().
 *
 * @param val the scalar value, possibly NULL
 * @param token the provenance token to multiply
 */
CREATE OR REPLACE FUNCTION provenance_semimod_nullable(val anyelement, token UUID)
  RETURNS UUID AS
  'provsql','provenance_semimod_nullable' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

-- ----------------------------------------------------------------------
-- 6c. Gate-building functions in C: same gates at the same addresses,
--     without the SPI statements of the PL/pgSQL versions.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
  'provsql','provenance_times' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_plus(tokens uuid[])
  RETURNS UUID AS
  'provsql','provenance_plus' LANGUAGE C COST 100 STRICT PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_semimod(val anyelement, token UUID)
  RETURNS UUID AS
  'provsql','provenance_semimod' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[],
    is_scalar boolean DEFAULT false)
  RETURNS agg_token AS
  'provsql','provenance_aggregate' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_monus(token1 UUID, token2 UUID)
  RETURNS UUID AS
  'provsql','provenance_monus' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_delta
  (token UUID)
  RETURNS UUID AS
  'provsql','provenance_delta' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_cmp(
  left_token  UUID,
  comparison_op OID,
  right_token UUID
)
RETURNS UUID AS
  'provsql','provenance_cmp' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION annotate(token UUID, extra TEXT) RETURNS UUID AS
  'provsql','annotate' LANGUAGE C COST 100 PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inversion_free_key(root TEXT, sec TEXT, factor INT)
  RETURNS TEXT AS
  'provsql','inversion_free_key' LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 6d. Planted gates are remembered by the session that plants them; the
--     store is no longer probed at canonical addresses.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION planted_scope(work_name text)
  RETURNS void AS
  'provsql','planted_scope' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION plant_canonical(
  work_name text, kind text, tokens uuid[], target uuid,
  info1 int, info2 int DEFAULT 0)
  RETURNS uuid AS
  'provsql','plant_canonical' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION eval_recursive(
  body_sql  text,
  work_name text,
  colnames  text,
  coldef    text,
  max_iter  int DEFAULT 1000)
  RETURNS void AS
$$
DECLARE
  changed   boolean;        -- circuit changed structurally this round
  set_stable boolean;       -- user-column tuple set unchanged this round
  iters     int := 0;
  new_count int;            -- rows in provsql_rec_new this round (INSERT ROW_COUNT)
  -- The derivations of a tuple can repeat through the tuple itself although
  -- the tuple set stabilises: on cyclic data, but also on acyclic data through
  -- a null-padded row that re-derives itself or a projection onto constants.
  -- The circuit then keeps growing, one summand per round, and only an
  -- absorptive class has a value for it: 1 ⊕ a = 1 gives x ⊕ x ⊗ y = x, so a
  -- derivation that extends another is absorbed by it, and every step of a
  -- cycle contracts -- a ⊖ b <= a by residuation, so a monus in the cycle is
  -- covered too, not only a product.  (Where the surplus derivation adds
  -- nothing at all, as a null-padded row re-deriving itself does, the same
  -- fact reads as idempotence, a ⊕ a = a.)  A
  -- minimal derivation cannot repeat a tuple, so it has depth <= (number of
  -- derivable tuples); after that many naive rounds the value equals the least
  -- fixpoint of an absorptive class, and the surplus derivations are absorbed
  -- at evaluation time.  We learn that bound from the tuple-set fixpoint: in
  -- an absorptive class we stop there and mark the tokens with the
  -- 'absorptive' assumption, so evaluation under a non-absorptive semiring
  -- refuses rather than silently returning a truncated value; in another class
  -- there is no value to give, and reaching the bound is what tells us so --
  -- the refusal comes then, rather than after max_iter rounds of building
  -- circuit for an answer that will not come.
  absorptive_mode boolean :=
    coalesce(current_setting('provsql.provenance', true), 'semiring')
      IN ('absorptive', 'boolean');
  truncated boolean := false; -- exited at the value fixpoint
  ntuples   int := NULL;    -- the bound above, set once the tuple set stabilises
BEGIN
  EXECUTE format('DROP TABLE IF EXISTS %I', work_name);
  DROP TABLE IF EXISTS provsql_rec_new;

  -- Tracked working table (carries provsql), initially empty, plus a scratch
  -- table of the same shape; both reused across rounds.
  EXECUTE format('CREATE TEMP TABLE %I (%s, provsql uuid) ON COMMIT DROP',
                 work_name, coldef);
  PERFORM provsql.planted_scope(work_name);
  EXECUTE format('CREATE TEMP TABLE provsql_rec_new (LIKE %I) ON COMMIT DROP',
                 work_name);

  LOOP
    iters := iters + 1;
    -- Hard safety bound (also catches genuinely unbounded recursion, e.g. an
    -- unbounded counter, where even the tuple set never stabilises).
    IF iters > max_iter THEN
      /* Not even the rows stop changing: the recursion derives tuples without
       * end (an unbounded counter), which SQL does not terminate on either.
       * A recursion whose rows settle while its derivations repeat exits at
       * the value fixpoint below, tagged, and never reaches this. */
      RAISE EXCEPTION 'ProvSQL: the rounds of this recursion do not reach a '
                      'fixpoint (after % of them): the rows it derives keep '
                      'changing, so there is no fixpoint to annotate -- plain '
                      'SQL does not terminate on such a recursion either',
                      max_iter
        USING ERRCODE = 'feature_not_supported',
              DETAIL = 'provsql-reason: recursion-no-fixpoint; scope: deliberate';
    END IF;

    -- One round of naive evaluation: re-run the CTE body over the current
    -- working table.  INSERT targets a tracked table, so ProvSQL fills provsql.
    -- Take the row count from the INSERT itself (counting provsql_rec_new directly would be
    -- an aggregate over a provenance-tracked table -> an agg_token).
    EXECUTE 'TRUNCATE provsql_rec_new';
    EXECUTE format('INSERT INTO provsql_rec_new(%s) %s', colnames, body_sql);
    GET DIAGNOSTICS new_count = ROW_COUNT;

    -- Exact structural fixpoint test (content-addressed tokens => set equality).
    EXECUTE format(
      'SELECT EXISTS((TABLE provsql_rec_new EXCEPT TABLE %1$I) UNION ALL (TABLE %1$I EXCEPT TABLE provsql_rec_new))',
      work_name) INTO changed;

    -- Learn the round bound from the tuple-set fixpoint (the set stabilises
    -- after finitely many rounds even where the derivations do not).
    IF ntuples IS NULL THEN
      EXECUTE format(
        'SELECT NOT EXISTS('
        || '(SELECT %2$s FROM provsql_rec_new EXCEPT SELECT %2$s FROM %1$I) UNION ALL '
        || '(SELECT %2$s FROM %1$I EXCEPT SELECT %2$s FROM provsql_rec_new))',
        work_name, colnames) INTO set_stable;
      IF set_stable THEN
        ntuples := new_count;
      END IF;
    END IF;

    -- Copy provsql_rec_new into the working table (tracked -> tracked carries the tokens).
    EXECUTE format('TRUNCATE %I', work_name);
    EXECUTE format('INSERT INTO %1$I(%2$s) SELECT %2$s FROM provsql_rec_new', work_name, colnames);

    -- Structural fixpoint: done (acyclic / fully converged) -- sound for any
    -- semiring.
    EXIT WHEN NOT changed;

    -- The derivations repeat through a tuple: the rows have settled, the
    -- circuit has not, and it will not (one summand per round from here on).
    -- The bound is the tuple-set fixpoint plus one confirming round, so that a
    -- recursion whose token depth merely lags the tuple-set saturation still
    -- exits through the structural test above, untagged.
    IF ntuples IS NOT NULL AND iters >= ntuples + 1 THEN
      IF absorptive_mode THEN
        -- The value of an absorptive class is reached: stop, tagged below.
        truncated := true;
        EXIT;
      END IF;
      /* No absorption: the annotation of such a tuple gains a term per round
       * and has no value, so there is nothing to return -- a refusal by what
       * the recursion means, not a limit of the driver. */
      RAISE EXCEPTION 'ProvSQL: the rounds of this recursion do not reach a '
                      'fixpoint (the rows settled after % of them, the '
                      'derivations did not): a tuple is derived through '
                      'itself, which cyclic data does, and so does a '
                      'null-padded row that re-derives itself or a projection '
                      'onto constants on acyclic data, so its annotation '
                      'gains a term at every round.  Only an absorptive '
                      'provenance class has a value for it (set '
                      'provsql.provenance to absorptive or to boolean)',
                      iters
        USING ERRCODE = 'feature_not_supported',
              DETAIL = 'provsql-reason: recursion-no-fixpoint; scope: deliberate';
    END IF;
  END LOOP;

  -- Tokens of a truncated fixpoint are sound only under absorptive
  -- evaluation: record that in the circuit itself.
  IF truncated THEN
    EXECUTE format(
      'UPDATE %I SET provsql = provsql.provenance_assume(provsql, ''absorptive'')',
      work_name);
  END IF;
END
$$ LANGUAGE plpgsql SET client_min_messages = warning;

CREATE OR REPLACE FUNCTION plant_reach_any_groups(
  work_name text,
  node_attribute text,
  member_rel regclass,
  member_attribute text,
  group_attribute text,
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  edge_quals text DEFAULT NULL,
  source_rel regclass DEFAULT NULL,
  source_rel_attribute text DEFAULT NULL,
  edge_sql text DEFAULT NULL,
  member_quals text DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  e record;
  grp record;
  m record;
  sv text[];
  st uuid[];
  sp double precision[];
  gids int[] := ARRAY[]::int[];
  mids int[] := ARRAY[]::int[];
  vid int;
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
    -- A tracked member relation would make the aggregated tokens
    -- per-row products, not the bare reach tokens: nothing to plant.
    IF EXISTS (SELECT 1 FROM pg_attribute
               WHERE attrelid = member_rel AND attname = 'provsql'
                 AND atttypid = 'uuid'::regtype AND NOT attisdropped) THEN
      RETURN;
    END IF;

    IF source_rel IS NOT NULL THEN
      SELECT g.source_values, g.source_tokens, g.source_probabilities
        INTO sv, st, sp
        FROM provsql.gather_reachability_sources(source_rel,
                                                 source_rel_attribute) g;
      IF sv IS NULL THEN
        sv := ARRAY[]::text[];
        st := ARRAY[]::uuid[];
        sp := ARRAY[]::float8[];
      END IF;
    ELSE
      sv := ARRAY[source_value];
      st := ARRAY['00000000-0000-0000-0000-000000000000'::uuid];
      sp := ARRAY[1.0::float8];
    END IF;

    e := provsql.gather_reachability_edges(edge_rel, source_attribute,
                                           destination_attribute,
                                           sv, edge_quals, edge_sql);

    -- The groups, replicating the user's join semantics: per group, the
    -- member vertices and the multiset of their reach tokens (with the
    -- multiplicity the join produces).  Single-member groups need no
    -- planting (provenance_plus passes a single token through).
    -- Two steps: materialise the joined rows with their per-row tokens
    -- (tracked CTAS, then strip the automatic provsql column), and only
    -- then aggregate the now-plain table -- aggregating provenance()
    -- inside a grouped tracked query would be rewritten as a
    -- provenance-aware aggregation, which is not what the planting
    -- needs.
    DROP TABLE IF EXISTS provsql_reach_any_flat_tmp;
    EXECUTE format(
      'CREATE TEMP TABLE provsql_reach_any_flat_tmp AS '
      || 'SELECT w.%1$I::text AS node_val, provsql.provenance() AS tok, '
      || '       t.%5$I AS grp_key '
      || 'FROM %2$I w JOIN %3$s t ON w.%1$I = t.%4$I'
      -- The member-relation filter restricts which members participate
      -- (deparsed table-qualified as t.column); the working table side
      -- carries no provenance distinction here.
      || coalesce(' WHERE ' || member_quals, ''),
      node_attribute, work_name, member_rel::text, member_attribute,
      group_attribute);
    PERFORM provsql.remove_provenance('provsql_reach_any_flat_tmp');
    DROP TABLE IF EXISTS provsql_reach_any_groups_tmp;
    CREATE TEMP TABLE provsql_reach_any_groups_tmp AS
      SELECT (row_number() OVER ())::int AS gid, members, toks FROM (
        SELECT array_agg(node_val) AS members, array_agg(tok) AS toks
        FROM provsql_reach_any_flat_tmp
        GROUP BY grp_key HAVING count(*) >= 2) g;
    DROP TABLE provsql_reach_any_flat_tmp;

    FOR grp IN SELECT gid, members FROM provsql_reach_any_groups_tmp LOOP
      FOR m IN SELECT DISTINCT unnest(grp.members) AS val LOOP
        vid := array_position(e.vertices, m.val);
        IF vid IS NOT NULL THEN
          gids := gids || grp.gid;
          mids := mids || vid;
        END IF;
      END LOOP;
    END LOOP;
    IF cardinality(gids) = 0 THEN
      DROP TABLE provsql_reach_any_groups_tmp;
      RETURN;
    END IF;

    FOR grp IN
      SELECT a.group_id, a.token AS any_token, t.toks
      FROM provsql.reachability_materialize_any(
             e.sources, e.destinations, e.tokens, e.probabilities,
             e.block_keys, e.block_indices, e.extra_ids, st, sp,
             directed, gids, mids) a
      JOIN provsql_reach_any_groups_tmp t ON t.gid = a.group_id
    LOOP
      PERFORM provsql.plant_canonical(work_name, 'plus', grp.toks,
                                      grp.any_token, 1);
    END LOOP;
    DROP TABLE provsql_reach_any_groups_tmp;
    IF verbosity >= 20 THEN
      -- Lift the function-level client_min_messages = warning for the
      -- one RAISE; the function-level SET restores the caller's value.
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: certified any-member gates planted for the aggregation of "%" by %.%',
        work_name, member_rel, group_attribute;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: any-member planting for "%" skipped (%)',
        work_name, SQLERRM;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  END;
END
-- No SET search_path: the deparsed edge subquery must resolve against
-- the caller's path; ProvSQL internals are schema-qualified.
$$ LANGUAGE plpgsql SET client_min_messages = warning;

CREATE OR REPLACE FUNCTION plant_reach_cover(
  work_name text,
  node_attribute text,
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  node_values text[],
  edge_quals text DEFAULT NULL,
  source_rel regclass DEFAULT NULL,
  source_rel_attribute text DEFAULT NULL,
  edge_sql text DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  e record;
  sv text[];
  st uuid[];
  sp double precision[];
  val text;
  vid int;
  vids int[] := ARRAY[]::int[];
  tok uuid;
  toks uuid[] := ARRAY[]::uuid[];
  cover_token uuid;
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
    IF source_rel IS NOT NULL THEN
      SELECT g.source_values, g.source_tokens, g.source_probabilities
        INTO sv, st, sp
        FROM provsql.gather_reachability_sources(source_rel,
                                                 source_rel_attribute) g;
      IF sv IS NULL THEN
        sv := ARRAY[]::text[];
        st := ARRAY[]::uuid[];
        sp := ARRAY[]::float8[];
      END IF;
    ELSE
      sv := ARRAY[source_value];
      st := ARRAY['00000000-0000-0000-0000-000000000000'::uuid];
      sp := ARRAY[1.0::float8];
    END IF;

    e := provsql.gather_reachability_edges(edge_rel, source_attribute,
                                           destination_attribute,
                                           sv, edge_quals, edge_sql);

    -- The bound vertices and their per-row reach tokens, with the
    -- multiplicity the self-join produces.  A vertex absent from the
    -- graph, or from the working table, means the join is empty: no
    -- row will exist, nothing to plant.
    FOREACH val IN ARRAY node_values LOOP
      vid := array_position(e.vertices, val);
      IF vid IS NULL THEN
        RETURN;
      END IF;
      vids := vids || vid;
      EXECUTE format('SELECT provsql FROM %I WHERE %I::text = $1',
                     work_name, node_attribute)
        INTO tok USING val;
      IF tok IS NULL THEN
        RETURN;
      END IF;
      toks := toks || tok;
    END LOOP;

    cover_token := provsql.reachability_materialize_cover(
      e.sources, e.destinations, e.tokens, e.probabilities,
      e.block_keys, e.block_indices, e.extra_ids, st, sp,
      directed, vids);

    PERFORM provsql.plant_canonical(work_name, 'times', toks, cover_token, 1);
    IF verbosity >= 20 THEN
      -- Lift the function-level client_min_messages = warning for the
      -- one RAISE; the function-level SET restores the caller's value.
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: certified all-members gate planted for the self-join of "%"',
        work_name;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      PERFORM set_config('client_min_messages', 'notice', true);
      RAISE NOTICE 'ProvSQL: all-members planting for "%" skipped (%)',
        work_name, SQLERRM;
      PERFORM set_config('client_min_messages', 'warning', true);
    END IF;
  END;
END
-- No SET search_path: the deparsed edge subquery must resolve against
-- the caller's path; ProvSQL internals are schema-qualified.
$$ LANGUAGE plpgsql SET client_min_messages = warning;

CREATE OR REPLACE FUNCTION eval_reachability(
  edge_rel regclass,
  source_attribute text,
  destination_attribute text,
  source_value text,
  directed boolean,
  work_name text,
  colnames text,
  coldef text,
  coltype text,
  body_sql text,
  edge_quals text DEFAULT NULL,
  source_rel regclass DEFAULT NULL,
  source_rel_attribute text DEFAULT NULL,
  edge_sql text DEFAULT NULL,
  hop_bound int DEFAULT NULL,
  hop_seed int DEFAULT NULL,
  hops_position int DEFAULT NULL)
  RETURNS void AS
$$
DECLARE
  e record;
  sv text[];
  st uuid[];
  sp double precision[];
  verbosity int := coalesce(current_setting('provsql.verbose_level', true)::int, 0);
BEGIN
  BEGIN
    IF source_rel IS NOT NULL THEN
      -- Multi-source: gather the source relation (probabilistic when
      -- tracked, certain otherwise).
      SELECT g.source_values, g.source_tokens, g.source_probabilities
        INTO sv, st, sp
        FROM provsql.gather_reachability_sources(source_rel,
                                                 source_rel_attribute) g;
      IF sv IS NULL THEN
        sv := ARRAY[]::text[];
        st := ARRAY[]::uuid[];
        sp := ARRAY[]::float8[];
      END IF;
    ELSE
      -- Constant base arm: one certain source.
      sv := ARRAY[source_value];
      st := ARRAY['00000000-0000-0000-0000-000000000000'::uuid];
      sp := ARRAY[1.0::float8];
    END IF;

    e := provsql.gather_reachability_edges(edge_rel, source_attribute,
                                           destination_attribute,
                                           sv, edge_quals, edge_sql);
    IF to_regclass(work_name) IS NOT NULL THEN
      EXECUTE format('DROP TABLE %I', work_name);
    END IF;
    EXECUTE format('CREATE TEMP TABLE %I (%s, provsql uuid)', work_name, coldef);
    PERFORM provsql.planted_scope(work_name);
    IF hop_bound IS NULL THEN
      EXECUTE format(
        'INSERT INTO %I SELECT ($1::text[])[m.vertex]::%s, m.token '
        || 'FROM provsql.reachability_materialize($2, $3, $4, $5, $6, $7, $8, $9, $10, $11) m',
        work_name, coltype)
        USING e.vertices, e.sources, e.destinations, e.tokens, e.probabilities,
              e.block_keys, e.block_indices, e.extra_ids, st, sp, directed;
    ELSE
      -- Hop-counting shape: one row per (vertex, walk length), the hop
      -- column in its CTE position.
      EXECUTE format(
        'INSERT INTO %I SELECT %s, m.token '
        || 'FROM provsql.reachability_materialize_hops($2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) m',
        work_name,
        CASE WHEN hops_position = 1
             THEN format('m.hops, ($1::text[])[m.vertex]::%s', coltype)
             ELSE format('($1::text[])[m.vertex]::%s, m.hops', coltype) END)
        USING e.vertices, e.sources, e.destinations, e.tokens, e.probabilities,
              e.block_keys, e.block_indices, e.extra_ids, st, sp, directed,
              hop_bound, hop_seed;
    END IF;
    IF verbosity >= 20 THEN
      RAISE NOTICE 'ProvSQL: recursive CTE "%" compiled along a tree decomposition of %',
        work_name, coalesce(edge_rel::text, 'the join-defined edge query');
    END IF;
  EXCEPTION WHEN OTHERS THEN
    IF verbosity >= 10 THEN
      /* Named as the user named the CTE: the working table carries a name of
         ours (provsql_rec_<cte>), which is no business of a message. */
      RAISE NOTICE 'ProvSQL: reachability route for "%" fell back to the generic fixpoint (%)',
        regexp_replace(work_name, '^provsql_rec_', ''), SQLERRM;
    END IF;
    PERFORM provsql.eval_recursive(body_sql, work_name, colnames, coldef);
  END;
END
$$ LANGUAGE plpgsql;

-- ----------------------------------------------------------------------
-- 6e. Gates created together with what they record: create_gate with infos
--     and text, and the builders that use it.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION create_gate(
  token UUID,
  type provenance_gate,
  children uuid[],
  info1 INT,
  info2 INT,
  extra TEXT)
  RETURNS void AS
  'provsql','create_gate' LANGUAGE C PARALLEL SAFE;

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
    PERFORM provsql.create_gate(new_tok, 'mulinput', ARRAY[new_key], r.ord, n, NULL);
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

CREATE OR REPLACE FUNCTION provenance_assume(token UUID, assumption TEXT)
  RETURNS UUID AS
  'provsql','provenance_assume' LANGUAGE C COST 100 PARALLEL SAFE;

CREATE OR REPLACE FUNCTION assume_boolean(token UUID) RETURNS UUID AS
  'provsql','assume_boolean' LANGUAGE C COST 100 PARALLEL SAFE;

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
    PERFORM provsql.create_gate(r.provsql_temp, 'mulinput', ARRAY[r.key_token],
                                r.within_group::int, r.group_size::int, NULL);
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

CREATE OR REPLACE FUNCTION provenance_project(token UUID, VARIADIC positions int[])
  RETURNS UUID AS
  'provsql','provenance_project' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_eq(token UUID, pos1 int, pos2 int)
  RETURNS UUID AS
  'provsql','provenance_eq' LANGUAGE C COST 100 PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_arith(
  op       INTEGER,
  children UUID[]
)
RETURNS UUID AS
  'provsql','provenance_arith' LANGUAGE C COST 100 STRICT PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION agg_value_gate(v numeric)
  RETURNS uuid AS
$$
DECLARE
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', v::text));
BEGIN
  PERFORM provsql.create_gate(token, 'value', NULL, NULL, NULL, v::text);
  RETURN token;
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_arith_make(op int, children uuid[], val numeric)
  RETURNS agg_token AS
$$
DECLARE
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('arith', op::text, children::text));
BEGIN
  PERFORM provsql.create_gate(token, 'arith', children, op, NULL, val::text);
  RETURN provsql.agg_token_make(token, val);
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION rv_parametric2(
    family text,
    p1_tok uuid, p1_lit double precision,
    p2_tok uuid, p2_lit double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
  wires uuid[] := ARRAY[]::uuid[];
  s1 text;
  s2 text;
BEGIN
  IF p1_tok IS NOT NULL THEN
    wires := wires || p1_tok;
    s1 := '$' || (array_length(wires, 1) - 1);
  ELSE
    IF NOT provsql.is_finite_float8(p1_lit) THEN
      RAISE EXCEPTION 'provsql.%: literal parameter must be finite (got %)',
        family, p1_lit;
    END IF;
    s1 := p1_lit::text;
  END IF;
  IF p2_tok IS NOT NULL THEN
    wires := wires || p2_tok;
    s2 := '$' || (array_length(wires, 1) - 1);
  ELSE
    IF NOT provsql.is_finite_float8(p2_lit) THEN
      RAISE EXCEPTION 'provsql.%: literal parameter must be finite (got %)',
        family, p2_lit;
    END IF;
    s2 := p2_lit::text;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', wires, NULL, NULL,
                              family || ':' || s1 || ',' || s2);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_parametric1(family text, p_tok uuid)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', ARRAY[p_tok], NULL, NULL, family || ':$0');
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION normal(mu double precision, sigma double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(sigma) THEN
    RAISE EXCEPTION 'provsql.normal: parameters must be finite (got mu=%, sigma=%)', mu, sigma;
  END IF;
  IF sigma < 0 THEN
    RAISE EXCEPTION 'provsql.normal: sigma must be non-negative (got %)', sigma;
  END IF;
  IF sigma = 0 THEN
    RETURN provsql.as_random(mu);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'normal:' || mu || ',' || sigma);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION logistic(mu double precision, s double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(s) THEN
    RAISE EXCEPTION 'provsql.logistic: parameters must be finite (got mu=%, s=%)', mu, s;
  END IF;
  IF s < 0 THEN
    RAISE EXCEPTION 'provsql.logistic: scale s must be non-negative (got %)', s;
  END IF;
  IF s = 0 THEN
    RETURN provsql.as_random(mu);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'logistic:' || mu || ',' || s);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION uniform(a double precision, b double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(a) OR NOT provsql.is_finite_float8(b) THEN
    RAISE EXCEPTION 'provsql.uniform: bounds must be finite (got a=%, b=%)', a, b;
  END IF;
  IF a > b THEN
    RAISE EXCEPTION 'provsql.uniform: a must be <= b (got a=%, b=%)', a, b;
  END IF;
  IF a = b THEN
    RETURN provsql.as_random(a);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'uniform:' || a || ',' || b);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION exponential(lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.exponential: lambda must be strictly positive (got %)', lambda;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'exponential:' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION erlang(k integer, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF k < 1 THEN
    RAISE EXCEPTION 'provsql.erlang: k must be >= 1 (got %)', k;
  END IF;
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.erlang: lambda must be strictly positive (got %)', lambda;
  END IF;
  IF k = 1 THEN
    RETURN provsql.exponential(lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'erlang:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gamma(k double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(k) THEN
    RAISE EXCEPTION 'provsql.gamma: k must be finite (got %)', k;
  END IF;
  IF k <= 0 THEN
    RAISE EXCEPTION 'provsql.gamma: k must be strictly positive (got %)', k;
  END IF;
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.gamma: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.gamma: lambda must be strictly positive (got %)', lambda;
  END IF;
  IF k = floor(k) AND k <= 2147483647 THEN
    RETURN provsql.erlang(k::integer, lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'gamma:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION lognormal(mu double precision, sigma double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(sigma) THEN
    RAISE EXCEPTION 'provsql.lognormal: parameters must be finite (got mu=%, sigma=%)', mu, sigma;
  END IF;
  IF sigma < 0 THEN
    RAISE EXCEPTION 'provsql.lognormal: sigma must be non-negative (got %)', sigma;
  END IF;
  IF sigma = 0 THEN
    RETURN provsql.as_random(exp(mu));
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'lognormal:' || mu || ',' || sigma);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION weibull(k double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(k) OR NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.weibull: parameters must be finite (got k=%, lambda=%)', k, lambda;
  END IF;
  IF k <= 0 OR lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.weibull: parameters must be strictly positive (got k=%, lambda=%)', k, lambda;
  END IF;
  IF k = 1 THEN
    RETURN provsql.exponential(1 / lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'weibull:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION pareto(xm double precision, alpha double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(xm) OR NOT provsql.is_finite_float8(alpha) THEN
    RAISE EXCEPTION 'provsql.pareto: parameters must be finite (got xm=%, alpha=%)', xm, alpha;
  END IF;
  IF xm <= 0 OR alpha <= 0 THEN
    RAISE EXCEPTION 'provsql.pareto: parameters must be strictly positive (got xm=%, alpha=%)', xm, alpha;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'pareto:' || xm || ',' || alpha);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gamma(alpha double precision, beta double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(alpha) OR NOT provsql.is_finite_float8(beta) THEN
    RAISE EXCEPTION 'provsql.inverse_gamma: parameters must be finite (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha <= 0 OR beta <= 0 THEN
    RAISE EXCEPTION 'provsql.inverse_gamma: parameters must be strictly positive (got alpha=%, beta=%)', alpha, beta;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'inverse_gamma:' || alpha || ',' || beta);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gaussian(mu double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.inverse_gaussian: parameters must be finite (got mu=%, lambda=%)', mu, lambda;
  END IF;
  IF mu <= 0 OR lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.inverse_gaussian: parameters must be strictly positive (got mu=%, lambda=%)', mu, lambda;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'inverse_gaussian:' || mu || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION beta(alpha double precision, beta double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(alpha) OR NOT provsql.is_finite_float8(beta) THEN
    RAISE EXCEPTION 'provsql.beta: parameters must be finite (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha <= 0 OR beta <= 0 THEN
    RAISE EXCEPTION 'provsql.beta: parameters must be strictly positive (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha = 1 AND beta = 1 THEN
    RETURN provsql.uniform(0, 1);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', NULL, NULL, NULL, 'beta:' || alpha || ',' || beta);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION categorical(
  probs    double precision[],
  outcomes double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  p_sum double precision := 0.0;
  i integer;
  key_token uuid;
  mix_token uuid;
  mul_token uuid;
  mul_tokens uuid[] := ARRAY[]::uuid[];
  mix_wires  uuid[];
  pi_i double precision;
  vi_i double precision;
BEGIN
  IF probs IS NULL OR outcomes IS NULL THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must be non-null';
  END IF;
  n := array_length(probs, 1);
  IF n IS NULL OR n < 1 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must be non-empty';
  END IF;
  IF array_length(outcomes, 1) <> n THEN
    RAISE EXCEPTION 'provsql.categorical: probs and outcomes must have the same length (got % and %)',
      n, array_length(outcomes, 1);
  END IF;

  FOR i IN 1..n LOOP
    pi_i := probs[i];
    vi_i := outcomes[i];
    -- PostgreSQL diverges from IEEE 754: NaN = NaN is TRUE there, so
    -- the canonical x <> x NaN test doesn't fire.  Compare against the
    -- literal 'NaN'::float8 instead, and reject ±Infinity for outcomes
    -- explicitly.
    IF pi_i IS NULL OR pi_i = 'NaN'::float8 OR pi_i < 0 OR pi_i > 1 THEN
      RAISE EXCEPTION 'provsql.categorical: probs[%] must be in [0,1] (got %)', i, pi_i;
    END IF;
    IF vi_i IS NULL OR vi_i = 'NaN'::float8
       OR vi_i = 'Infinity'::float8 OR vi_i = '-Infinity'::float8 THEN
      RAISE EXCEPTION 'provsql.categorical: outcomes[%] must be finite (got %)', i, vi_i;
    END IF;
    p_sum := p_sum + pi_i;
  END LOOP;
  IF abs(p_sum - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'provsql.categorical: probs must sum to 1 within 1e-9 (got %)', p_sum;
  END IF;

  -- Degenerate case: exactly one positive-mass outcome (the rest are
  -- zero).  The "categorical" is then a Dirac point mass; skip the
  -- block-allocation entirely and return @c as_random(v), which yields
  -- a shared, v5-keyed gate_value -- exactly what downstream
  -- evaluators (rv_moment, AnalyticEvaluator, rv_support) treat
  -- specially.  Saves a key gate and a mulinput per call, and lets
  -- two calls to @c categorical({1.0}, {v}) collide on the same
  -- gate_value UUID instead of producing distinct anonymous blocks.
  DECLARE
    nb_positive integer := 0;
    only_idx    integer := 0;
  BEGIN
    FOR i IN 1..n LOOP
      IF probs[i] > 0.0 THEN
        nb_positive := nb_positive + 1;
        only_idx := i;
      END IF;
    END LOOP;
    IF nb_positive = 1 THEN
      RETURN provsql.as_random(outcomes[only_idx]);
    END IF;
  END;

  -- Mint the block's key anchor.  Probability 1.0 matches the
  -- joint-table convention: the categorical mass lives on the
  -- mulinputs, the key just identifies the block.
  key_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(key_token, 'input');
  PERFORM provsql.set_prob(key_token, 1.0);

  -- One mulinput per positive-probability outcome.  Zero-probability
  -- entries contribute no mass and are skipped: the gate_mixture's
  -- wire vector is otherwise polluted with no-op leaves.
  FOR i IN 1..n LOOP
    pi_i := probs[i];
    IF pi_i <= 0.0 THEN CONTINUE; END IF;
    mul_token := public.uuid_generate_v4();
    PERFORM provsql.create_gate(mul_token, 'mulinput', ARRAY[key_token],
                                i - 1, NULL, outcomes[i]::text);
    PERFORM provsql.set_prob(mul_token, pi_i);
    mul_tokens := mul_tokens || mul_token;
  END LOOP;

  mix_wires := ARRAY[key_token] || mul_tokens;
  mix_token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(mix_token, 'mixture', mix_wires);
  RETURN provsql.random_variable_make(mix_token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION as_random(c double precision)
  RETURNS random_variable AS
$$
DECLARE
  -- Canonicalise -0.0 to +0.0: IEEE 754 defines x + 0.0 = +0.0 for
  -- both signed zeros, and is identity for finite, NaN, and ±Infinity.
  -- Without this, as_random(-0.0) and as_random(+0.0) would produce
  -- different gate UUIDs (their CAST AS VARCHAR text representations
  -- differ: '-0' vs '0') even though they denote the same constant.
  c_canon double precision := c + 0.0;
  c_text varchar := CAST(c_canon AS VARCHAR);
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('value', c_text));
BEGIN
  PERFORM provsql.create_gate(token, 'value', NULL, NULL, NULL, c_text);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION observe(x random_variable, datum double precision)
  RETURNS uuid AS
$$
DECLARE
  leaf uuid := (x)::uuid;
  result uuid;
BEGIN
  IF provsql.get_gate_type(leaf) <> 'rv' THEN
    RAISE EXCEPTION 'provsql.observe: the argument must be a bare '
      'random-variable leaf (a gate_rv), got a % gate', provsql.get_gate_type(leaf)
      USING HINT = 'observe binds a datum to a single distribution leaf; '
        'observing a derived quantity (a sum, product, or comparison) needs '
        'a change-of-variables density and is out of scope.';
  END IF;
  IF NOT provsql.is_finite_float8(datum) THEN
    RAISE EXCEPTION 'provsql.observe: datum must be finite (got %)', datum;
  END IF;
  result := public.uuid_generate_v4();
  PERFORM provsql.create_gate(result, 'observe', ARRAY[leaf], NULL, NULL, datum::text);
  RETURN result;
END
$$ LANGUAGE plpgsql VOLATILE
   SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_percentile_make(fraction double precision,
                                              pairs uuid[])
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF fraction IS NULL THEN
    RETURN NULL;
  END IF;
  IF fraction < 0 OR fraction > 1 THEN
    RAISE EXCEPTION
      'percentile_cont: fraction must be between 0 and 1 (got %)', fraction;
  END IF;
  token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('arith', '10', pairs::text, fraction::text));
  -- 10 = PROVSQL_ARITH_PERCENTILE
  PERFORM create_gate(token, 'arith', pairs, 10, NULL, fraction::text);
  RETURN random_variable_make(token);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

-- ----------------------------------------------------------------------
-- 6f. A gate's infos and text are given when it is created; nothing sets
--     them afterwards.
-- ----------------------------------------------------------------------

DROP FUNCTION IF EXISTS set_infos(uuid, int, int);
DROP FUNCTION IF EXISTS set_extra(uuid, text);

-- ----------------------------------------------------------------------
-- 6g. rank(), row_number() and dense_rank() over provenance-tracked
--     relations: row_number() is tracked as rank(),
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION row_number_as_rank(rank agg_token, row_number bigint)
  RETURNS agg_token
  AS 'provsql','row_number_as_rank' LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- and dense_rank() counts the distinct values before the row.

CREATE OR REPLACE FUNCTION window_distinct_tokens(vals anyarray, tokens uuid[])
  RETURNS uuid[] AS
$$
  SELECT array_agg(s ORDER BY s)
  FROM (SELECT provsql.provenance_semimod(1, provsql.provenance_plus(array_agg(tokens[i]))) AS s
        FROM generate_subscripts(vals, 1) AS i GROUP BY vals[i]) g
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 6h. ORDER BY ... LIMIT k is tracked as the filter of a rank; LIMIT
--     plain(k) keeps the truncation of the actual result.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION plain(value anyelement)
  RETURNS anyelement AS
$$ SELECT value $$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 6i. A boolean aggregate (bool_or, bool_and, every) in a boolean context
--     is cast back to boolean, as the others are to their type.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION agg_token_to_bool(agg_token)
  RETURNS boolean
  AS 'provsql','agg_token_to_bool' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE CAST (agg_token AS boolean) WITH FUNCTION agg_token_to_bool(agg_token) AS ASSIGNMENT;

-- ----------------------------------------------------------------------
-- 6j. Integer division of aggregates keeps SQL's truncation in the
--     displayed value.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION agg_token_intdiv(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(11, ARRAY[(a)::uuid, (b)::uuid],
     trunc(provsql.agg_token_value(a) / NULLIF(provsql.agg_token_value(b), 0))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_token_intdiv_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(11, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     trunc(provsql.agg_token_value(a) / NULLIF(b, 0))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION numeric_intdiv_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(11, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     trunc(a / NULLIF(provsql.agg_token_value(b), 0))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;


-- ----------------------------------------------------------------------
-- 6k. The displayed value of an aggregate reads only the rows that hold
--     in the database as it is (plain_truth); sr_boolean's mapping is
--     optional, every leaf being true without one.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION sr_boolean(token ANYELEMENT, token2value regclass = NULL)
  RETURNS BOOLEAN AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN NULL;
  END IF;
  RETURN provsql.provenance_evaluate_compiled(
    token,
    token2value,
    'boolean',
    TRUE
  );
END
$$ LANGUAGE plpgsql CALLED ON NULL INPUT PARALLEL SAFE STABLE;

CREATE FUNCTION plain_truth(token uuid)
  RETURNS boolean AS
  'provsql', 'plain_truth' LANGUAGE C PARALLEL SAFE STABLE;

-- ----------------------------------------------------------------------
-- 6l. ORDER BY on an aggregate result sorts on its value.
-- ----------------------------------------------------------------------
CREATE FUNCTION agg_token_plain_text(agg_token)
  RETURNS text
  AS 'provsql','agg_token_plain_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/** @brief The children of the aggregation gate @p token, one per
 *  contribution, to explode an aggregate result into rows (a join on it,
 *  @c explode_table).  Only for @c choose(), whose value is one of its
 *  contributions; any other aggregate is refused, its value being none of
 *  them (@c count(*) contributes a 1 per row) (internal use). */
CREATE FUNCTION agg_token_explode_children(token uuid)
  RETURNS uuid[] AS
$$
BEGIN
  IF provsql.get_gate_type(token) <> 'agg' THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = 'ProvSQL: only the result of an aggregate can be exploded '
                'into rows',
      DETAIL = 'provsql-reason: explode-not-an-aggregate; scope: deliberate';
  END IF;
  IF (provsql.get_infos(token)).info1 <>
       'provsql.choose(anyelement)'::regprocedure::oid THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = format('ProvSQL: the result of %s() cannot be exploded into '
                       'rows, one per value it aggregates: only that of '
                       'choose() is one of them; compare it in a HAVING '
                       'clause, or cast it explicitly (::bigint, ...) to '
                       'read its plain value',
                       (SELECT proname FROM pg_catalog.pg_proc
                        WHERE oid = (provsql.get_infos(token)).info1)),
      DETAIL = 'provsql-reason: explode-rows-aggregate-kind; scope: gap';
  END IF;
  RETURN provsql.get_children(token);
END
$$ LANGUAGE plpgsql STABLE PARALLEL SAFE;

/**
 * @brief The contribution of an aggregate result to an aggregate of another
 *        kind over it (internal)
 *
 * For a row of token @p token whose value @p val is the result of an
 * aggregate of another kind than the one aggregating it (an @c avg of a
 * @c count, a @c max of a @c sum, an aggregate of an arithmetic expression
 * over aggregates): @c semimod(g, token), where @c g is the inner
 * aggregate's own gate.  Its value is one per possible world, not one value
 * of the database, so the evaluators that read a value per world resolve it
 * and the closed forms decline it.
 */
CREATE FUNCTION provenance_semimod_nested(val agg_token, token uuid)
  RETURNS uuid AS
  'provsql','provenance_semimod_nested' LANGUAGE C PARALLEL SAFE IMMUTABLE;

-- round / floor / ceil / abs of an agg_token ---------------------------
-- Named as the SQL functions they stand for, so that the rewriter's
-- re-resolution of a call whose argument became an agg_token finds them
-- (try_swap_agg_func), exactly as the operators above are found.  They compute
-- in numeric, as the operators do, and the gate records the operation so the
-- value is read per possible world; the moment evaluators sample them, since
-- rounding does not commute with expectation.
/** @brief round(agg_token) (gate_arith ROUND). */
CREATE OR REPLACE FUNCTION round(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(12, ARRAY[(a)::uuid],
     pg_catalog.round(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief round(agg_token, integer): to @p d decimal digits. */
CREATE OR REPLACE FUNCTION round(a agg_token, d integer)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(12,
     ARRAY[(a)::uuid, provsql.agg_value_gate(d::numeric)],
     pg_catalog.round(provsql.agg_token_value(a), d)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief floor(agg_token) (gate_arith FLOOR). */
CREATE OR REPLACE FUNCTION floor(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(13, ARRAY[(a)::uuid],
     pg_catalog.floor(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief ceil(agg_token) (gate_arith CEIL). */
CREATE OR REPLACE FUNCTION ceil(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(14, ARRAY[(a)::uuid],
     pg_catalog.ceil(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief ceiling(agg_token), the SQL synonym of ceil. */
CREATE OR REPLACE FUNCTION ceiling(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(14, ARRAY[(a)::uuid],
     pg_catalog.ceil(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief abs(agg_token) (gate_arith ABS). */
CREATE OR REPLACE FUNCTION abs(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(15, ARRAY[(a)::uuid],
     pg_catalog.abs(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;


/**
 * @brief The contributions of an aggregate result to an aggregate of the
 *        same kind over it (internal)
 *
 * For a row of token @p token whose value @p val is the result of
 * @c sum / @c count / @c max / @c min over a group, aggregated again by
 * @c sum / @c max / @c min: the contributions @c semimod(v_i, token ⊗ k_i)
 * of the group's own contributions @c semimod(v_i, k_i), so that
 * @c sum(sum(x)) is @c sum(x) over the rows of the groups -- semimodule
 * scalar multiplication, in every semiring.  Collected by
 * @c provenance_contributions_cat.
 */
CREATE FUNCTION provenance_semimod_flat(val agg_token, token uuid)
  RETURNS uuid[] AS
  'provsql','provenance_semimod_flat' LANGUAGE C PARALLEL SAFE IMMUTABLE;

/** @brief Concatenation of the arrays of contributions of
 *  @c provenance_semimod_flat (internal) */
CREATE AGGREGATE provenance_contributions_cat(uuid[]) (
  SFUNC = array_cat,
  STYPE = uuid[],
  INITCOND = '{}'
);

-- The value gate of the NULL value, created like the zero and one gates
SELECT create_gate(gate_null(), 'value', NULL, NULL, NULL, 'NULL');
-- A CASE over aggregates: the NULL value is never defined, and a NULL value
-- is displayed as NULL
CREATE OR REPLACE FUNCTION agg_defined_event(token uuid)
  RETURNS uuid AS $$
DECLARE
  gt provenance_gate := get_gate_type(token);
  fname varchar;
  toks uuid[];
  wires uuid[];
  nw integer;
  m integer;
  i integer;
  running_neg uuid := gate_one();
  parts uuid[] := '{}';
BEGIN
  IF token = gate_null() THEN
    RETURN gate_zero();     -- the NULL value: never defined
  END IF;
  IF gt = 'agg' THEN
    SELECT proname INTO fname
      FROM pg_proc WHERE oid = (get_infos(token)).info1;
    -- A scalar COUNT has a row in every world, counting a real 0 over none;
    -- every other aggregate (a SUM over no row is SQL NULL, a grouped one has
    -- no row at all) is defined only where a contributing row is.
    IF fname = 'count' AND (get_infos(token)).info2 < 0 THEN
      RETURN gate_one();
    END IF;
    SELECT array_agg((get_children(c))[1]) INTO toks
      FROM unnest(get_children(token)) AS c;
    IF toks IS NULL THEN
      RETURN gate_zero();   -- structurally empty aggregate: never defined
    END IF;
    RETURN provenance_plus(toks);
  ELSIF gt = 'case' THEN
    wires := get_children(token);
    nw := array_length(wires, 1);
    m := (nw - 1) / 2;
    FOR i IN 1..m LOOP
      parts := parts || provenance_times(
        running_neg, wires[2 * i - 1],
        agg_defined_event(wires[2 * i]));
      running_neg := provenance_times(running_neg,
                                      provenance_not(wires[2 * i - 1]));
    END LOOP;
    parts := parts || provenance_times(running_neg,
                                       agg_defined_event(wires[nw]));
    RETURN provenance_plus(parts);
  END IF;
  -- value / arith / anything else: a value exists in every world.
  RETURN gate_one();
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_case(
  children UUID[]
)
RETURNS agg_token AS
$$
  SELECT format('( %s , %s )', t::text,
                coalesce(provsql.agg_gate_value(t)::text, ''))::provsql.agg_token
  FROM (SELECT provsql.provenance_case(children) AS t) AS s;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

-- explode_table explodes the result of choose() only
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
        FROM UNNEST(provsql.agg_token_explode_children(%3$I)) AS sm
    ) AS sub', _nsp, _tbl, agg_token);
    EXECUTE format('DROP TABLE %I.%I', _nsp, _tbl);
    EXECUTE format('ALTER TABLE %I.temp_exploded DROP COLUMN %I, DROP COLUMN provsql', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_t TO %I', _nsp, agg_token);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME COLUMN new_provsql TO provsql', _nsp);
    EXECUTE format('ALTER TABLE %I.temp_exploded RENAME TO %I', _nsp, _tbl);
END;
$$ LANGUAGE plpgsql;

/** @brief The values the aggregate result @p token takes over the possible
 *  worlds, as text, to explode that result into one row per value: the value
 *  of an aggregate read as data (a GROUP BY key, a DISTINCT, an arm of a set
 *  operation).  The planner annotates the row of a value @c v with the
 *  comparison gate @c [token @c = @c v], so that the rows of one group are
 *  pairwise exclusive and exactly one of them is in each world where the
 *  group is.
 *
 *  A @c count() takes every number of its contributions, a @c min(), a
 *  @c max() and a @c choose() one of their contributed values.  Any other
 *  aggregate is refused (SQLSTATE 0A000): the values of a @c sum() are its
 *  subset sums, those of a @c string_agg() one per ordering, and reading
 *  them off the contributions one by one would be wrong.  A NULL
 *  contribution is refused as well: whether the result is NULL is then a
 *  value of its own, which a comparison cannot express (internal use). */
CREATE FUNCTION agg_possible_values(input anyelement,
                                    with_null boolean DEFAULT false)
  RETURNS text[] AS
$$
DECLARE
  max_values CONSTANT int := 1000;  -- an explosion multiplies the rows
  token   uuid;
  fn      text;
  ns      text;
  vals    text[];
  n       int;
  counted int;
  first   int;
  sums    numeric[];
  one     numeric;
  scalar_agg boolean;
BEGIN
  /* The planner hands the aggregate result itself (an agg_token), whatever
   * the type the query declares for that column. */
  IF pg_typeof(input) <> 'provsql.agg_token'::regtype THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = 'ProvSQL: only the result of an aggregate can be exploded '
                'into one row per value it takes over the possible worlds',
      DETAIL = 'provsql-reason: explode-not-an-aggregate; scope: deliberate';
  END IF;
  token := input::uuid;
  IF provsql.get_gate_type(token) <> 'agg' THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = 'ProvSQL: an arithmetic expression over aggregate results '
                'cannot be exploded into one row per value it takes over '
                'the possible worlds',
      DETAIL = 'provsql-reason: explode-arithmetic; scope: gap';
  END IF;
  SELECT p.proname, s.nspname INTO fn, ns
  FROM pg_catalog.pg_proc p
       JOIN pg_catalog.pg_namespace s ON s.oid = p.pronamespace
  WHERE p.oid = (provsql.get_infos(token)).info1;

  vals := ARRAY(SELECT provsql.get_extra((provsql.get_children(sm))[2])
                FROM unnest(provsql.get_children(token)) AS sm);
  n := coalesce(array_length(vals, 1), 0);
  /* NULL is a value of the aggregate like any other -- the one it takes where
   * no row contributes, which an aggregation over the whole table reaches in
   * the world holding none of its rows (a grouped one has no row there at all).
   * Whether to offer it is the caller's to decide: the rewriting asks for it
   * only where it can annotate that row with the aggregate having no value,
   * and never for a count, which is 0 rather than NULL over no row. */
  scalar_agg := with_null;

  IF (ns, fn) = ('pg_catalog', 'count') THEN
    /* A count contributes 1 per row it counts and 0 per row it does not -- a
     * row whose value is NULL, as the null-padded row of an outer join is --
     * so its values are the numbers of rows counted, up to how many there are
     * to count.  Zero is one of them where a row that is not counted can be
     * the only one there, or where the aggregation is over the whole table; a
     * group of counted rows only has none of its rows in no world, a group
     * being no group without a row. */
    counted := (SELECT count(*) FROM unnest(vals) AS v
                WHERE v IS NOT NULL AND v <> '0');
    first := CASE WHEN (provsql.get_infos(token)).info2 < 0 OR counted < n
                  THEN 0 ELSE 1 END;
    IF counted + 1 - first > max_values THEN
      RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
        MESSAGE = format('ProvSQL: the result of %s() aggregates %s rows, '
                         'so it takes too many values over the possible '
                         'worlds to explode it into one row per value',
                         fn, n),
        HINT = 'cast it explicitly (::bigint, ...) to read its plain value',
      DETAIL = 'provsql-reason: explode-too-many-values; scope: gap';
    END IF;
    RETURN ARRAY(SELECT i::text
                 FROM generate_series(least(first, counted), counted) AS i);
  END IF;

  IF (ns, fn) NOT IN (('pg_catalog', 'min'), ('pg_catalog', 'max'),
                      ('pg_catalog', 'sum'), ('provsql', 'choose')) THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = format('ProvSQL: the result of %s() cannot be exploded into '
                       'one row per value it takes over the possible worlds: '
                       'only count(), min(), max(), sum() and choose() have '
                       'values that can be enumerated', fn),
      HINT = 'compare it in a HAVING clause, or cast it explicitly '
             '(::bigint, ...) to read its plain value',
      DETAIL = 'provsql-reason: explode-aggregate-kind; scope: gap';
  END IF;

  IF EXISTS (SELECT 1 FROM unnest(vals) AS v WHERE v IS NULL) THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = format('ProvSQL: the result of %s() aggregates a NULL value, '
                       'so it cannot be exploded into one row per value it '
                       'takes over the possible worlds: a comparison with '
                       'NULL does not say that the result is NULL', fn),
      DETAIL = 'provsql-reason: explode-null-value; scope: gap';
  END IF;

  IF (ns, fn) = ('pg_catalog', 'sum') THEN
    /* The value of a sum is the sum of the rows that are there, so its values
     * are its subset sums, reached by adding the contributions one at a time,
     * equal sums collapsing (three rows of 1 take three values, not eight).
     * The empty subset is the NULL above, which a scalar aggregation takes and
     * a grouped one does not (a group without a row is no group).  The planner only sends sums over an
     * integer column here, whose subset sums the evaluator's own arithmetic
     * reaches exactly. */
    sums := ARRAY[]::numeric[];
    FOREACH one IN ARRAY ARRAY(SELECT v::numeric FROM unnest(vals) AS v) LOOP
      sums := ARRAY(SELECT DISTINCT s FROM
                      (SELECT unnest(sums) AS s
                       UNION ALL SELECT one
                       UNION ALL SELECT unnest(sums) + one) AS u);
      IF array_length(sums, 1) > max_values THEN
        RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
          MESSAGE = format('ProvSQL: the result of %s() takes more than %s '
                           'values over the possible worlds, too many to '
                           'explode it into one row per value', fn,
                           max_values),
          HINT = 'cast it explicitly (::numeric, ...) to read its plain value',
      DETAIL = 'provsql-reason: explode-too-many-values; scope: gap';
      END IF;
    END LOOP;
    RETURN ARRAY(SELECT s::text FROM unnest(sums) AS s ORDER BY s)
           || CASE WHEN scalar_agg THEN ARRAY[NULL::text]
                   ELSE ARRAY[]::text[] END;
  END IF;

  IF n > max_values THEN
    RAISE EXCEPTION USING ERRCODE = 'feature_not_supported',
      MESSAGE = format('ProvSQL: the result of %s() aggregates %s rows, so '
                       'it takes too many values over the possible worlds to '
                       'explode it into one row per value', fn, n),
      HINT = 'cast it explicitly (::bigint, ...) to read its plain value',
      DETAIL = 'provsql-reason: explode-too-many-values; scope: gap';
  END IF;

  /* One row per distinct contributed value: each is the minimum (maximum,
   * choice) of the world where only its own row is. */
  RETURN ARRAY(SELECT DISTINCT v FROM unnest(vals) AS v ORDER BY v)
         || CASE WHEN scalar_agg THEN ARRAY[NULL::text]
                 ELSE ARRAY[]::text[] END;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE;

/** @brief Value of an agg_token as text, NULL for a NULL value, without the
 *  provenance-loss warning of the public casts: the value of an aggregate
 *  result read as a plain value where ProvSQL casts it (in a function, an
 *  operator, a comparison), which the planner reports once as evaluated as
 *  plain SQL (internal use). */
CREATE FUNCTION agg_token_frozen_value(agg_token)
  RETURNS text
  AS 'provsql','agg_token_plain_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ----------------------------------------------------------------------
-- 6m. expected / moment of AVG conditioned on its group existing take the
--     exact route, as the unconditional moment does.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION agg_raw_moment(
  token agg_token,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  pair_children uuid[];
  n integer;
  i integer;
  j integer;
  vals float8[];
  toks uuid[];
  total float8;
  total_probability float8;
  tup integer[];
  d integer;
  prod_v float8;
  distinct_tok uuid[];
  conj_token uuid;
  prob float8;
  sign_max float8;
  is_scalar boolean;
  defined_tok uuid;
BEGIN
  IF token IS NULL OR k IS NULL THEN
    RETURN NULL;
  END IF;
  IF k < 0 THEN
    RAISE EXCEPTION 'agg_raw_moment(): k must be non-negative (got %)', k;
  END IF;

  -- Aggregate-carrier CASE (a gate_case over aggregate branches): a first-match
  -- guarded selection.  The moment is CONDITIONAL on the CASE's value being
  -- defined (NULL only when it never is, mirroring the MIN/MAX convention):
  --   E[pick^k | defined ∧ prov]
  --     = Σ_i P(region_i ∧ def_i) · E[value_i^k | region_i ∧ def_i]
  --       / Σ_i P(region_i ∧ def_i),
  -- where region_i = (¬g_1 ∧ … ∧ ¬g_{i-1}) ∧ g_i ∧ prov is the world set that
  -- selects branch i (the default's region is "all guards false") and def_i is
  -- the branch's defined event (agg_defined_event: gate_one for sum / count /
  -- constants, "some row present" for min / max / avg, recursive for a nested
  -- CASE).  Both factors are exact: probability() over the region ∧ def event,
  -- and the conditional aggregate moment (a recursive agg_raw_moment on the
  -- branch aggregate, which conditions on its own definedness within the
  -- region, so the two factors weigh the same worlds).  The regions are
  -- mutually exclusive, so the terms sum with no inclusion-exclusion, and
  -- correlation between a guard and its branch (shared input tuples) is
  -- carried by the conditioning, exactly as HAVING carries it.  When every
  -- branch is defined everywhere, the defined mass equals P(prov) and the
  -- formula reduces to the plain region-weighted sum.
  IF get_gate_type(token) = 'case' THEN
    IF k = 0 THEN
      RETURN 1;
    END IF;
    DECLARE
      wires uuid[] := get_children(token);
      nw integer := array_length(get_children(token), 1);
      m integer := (array_length(get_children(token), 1) - 1) / 2;
      running_neg uuid := gate_one();
      region_full uuid;
      prov_p float8;
      p float8;
      total float8 := 0;
      def_mass float8 := 0;
      ci integer;
      vuid uuid;
      bm float8;
    BEGIN
      prov_p := probability(prov);
      IF prov_p IS NULL OR prov_p <= 0 THEN
        RETURN NULL;   -- impossible conditioning event
      END IF;
      -- Branches 1..m are the guarded WHENs; branch m+1 is the ELSE default,
      -- whose region is "all guards false".
      FOR ci IN 1 .. m + 1 LOOP
        IF ci <= m THEN
          region_full := provenance_times(running_neg, wires[2 * ci - 1], prov);
          vuid := wires[2 * ci];
          running_neg :=
            provenance_times(running_neg, provenance_not(wires[2 * ci - 1]));
        ELSE
          region_full := provenance_times(running_neg, prov);
          vuid := wires[nw];
        END IF;
        p := probability(provenance_times(region_full,
                                          agg_defined_event(vuid)));
        IF p > 0 THEN
          -- E[value_i^k | region_i ∧ def_i]: a constant branch is a Dirac
          -- (c^k, exact); a single aggregate or nested CASE is exact via
          -- agg_raw_moment (whose MIN/MAX/CASE arms condition on their own
          -- definedness within the region); an arithmetic / composite branch
          -- takes the Monte-Carlo scalar path (which composes with the
          -- aggregate leaves).
          IF get_gate_type(vuid) = 'value' THEN
            bm := power(CAST(get_extra(vuid) AS float8), k);
          ELSIF get_gate_type(vuid) IN ('agg', 'case') THEN
            bm := agg_raw_moment(agg_token_make(vuid, 0), k, region_full,
                                 method, arguments);
          ELSE
            bm := rv_moment(vuid, k, false, region_full);
          END IF;
          total := total + p * bm;
          def_mass := def_mass + p;
        END IF;
      END LOOP;
      IF def_mass <= 0 THEN
        RETURN NULL;   -- the CASE's value is never defined under prov
      END IF;
      RETURN total / def_mass;
    END;
  END IF;

  IF get_gate_type(token) <> 'agg' THEN
    IF get_gate_type(token) IN ('arith', 'conditioned') THEN
      -- An arithmetic combination of aggregates (SUM(x) + SUM(y), SUM(x) / 2),
      -- or a conditioning of one: the scalar evaluator, exact over the
      -- possible worlds of few inputs, sampled otherwise
      RETURN rv_moment((token)::uuid, k, false, prov);
    ELSE
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation',
      DETAIL = 'provsql-reason: moment-not-an-aggregate; scope: deliberate';
    END IF;
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  -- A contribution whose value is itself an aggregate (an avg of a count, a
  -- max of a sum, an aggregate of an arithmetic expression over aggregates:
  -- provenance_semimod_nested) takes one value per possible world, not a
  -- constant read off its gate, so none of the closed forms below applies to
  -- it.  The scalar evaluator reads such a value in every world.
  IF EXISTS (SELECT 1 FROM unnest(child_pairs) AS c
               WHERE get_gate_type((get_children(c))[2]) <> 'value') THEN
    RETURN rv_moment((token)::uuid, k, false, prov);
  END IF;

  IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
    -- count(*) and count(col) both keep the COUNT identity at the gate level,
    -- their value being the SUM of per-row 1 / 0-or-1 indicators, so their
    -- moments are computed exactly like SUM.
    --
    -- The value exists only where a contributing row does: SUM over no row is
    -- SQL NULL, and a grouped aggregation has no row there at all.  So the
    -- moment conditions on that event, as the MIN / MAX arms do, and
    -- @c expected(sum(x)) of a group equals @c expected(sum(x), provenance()).
    -- A scalar COUNT is the exception: its row is always there and counts a
    -- real 0 over no row.
    is_scalar := (get_infos(token)).info2 < 0;   -- the scalar flag, high bit

    -- Extract per-child token + value arrays.
    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;
    defined_tok := CASE
      WHEN aggregation_function = 'count' AND is_scalar THEN gate_one()
      WHEN n = 0 THEN gate_zero()
      ELSE provenance_plus(toks) END;

    IF n = 0 THEN
      -- No contributing row at all: a real 0 for a scalar COUNT, SQL NULL
      -- (never defined) otherwise.
      RETURN CASE WHEN defined_tok = gate_one() THEN 0 ELSE NULL END;
    END IF;

    -- Collapsed fast path: a correlated COUNT / SUM whose per-row selection
    -- events share a single continuous latent has an O(G·n) 1-D quadrature,
    -- vastly cheaper than the O(n^k) tuple enumeration below (which is the
    -- O(n^2) pair-probability bottleneck for the variance).  Only fires
    -- unconditionally (prov = one) and for k in {1, 2}; agg_collapsed_moment
    -- returns NULL when the shared-latent pattern does not match, and we
    -- fall through to the exact enumeration.  Its moment counts the worlds
    -- without a row as 0, which contribute nothing, so it only needs the
    -- conditional normalisation below.
    IF prov = gate_one() AND k <= 2 THEN
      total := agg_collapsed_moment((token)::uuid, k);
      IF total IS NOT NULL THEN
        IF defined_tok = gate_one() THEN
          RETURN total;
        END IF;
        prob := probability_evaluate(defined_tok, method, arguments);
        IF prob IS NULL OR prob <= 0 THEN
          RETURN NULL;
        END IF;
        RETURN total / prob;
      END IF;
    END IF;

    -- Enumerate all k-tuples (i_1, ..., i_k) in {1..n}^k.  tup is the
    -- current tuple; we step through them in lexicographic order.
    total := 0;
    tup := array_fill(1, ARRAY[k]);
    LOOP
      prod_v := 1;
      FOR j IN 1..k LOOP
        prod_v := prod_v * vals[tup[j]];
      END LOOP;

      SELECT array_agg(DISTINCT toks[idx]) INTO distinct_tok
        FROM unnest(tup) AS idx;

      IF prov <> gate_one() THEN
        distinct_tok := distinct_tok || prov;
      END IF;
      conj_token := provenance_times(VARIADIC distinct_tok);
      prob := probability_evaluate(conj_token, method, arguments);

      total := total + prod_v * prob;

      d := k;
      WHILE d >= 1 AND tup[d] = n LOOP
        tup[d] := 1;
        d := d - 1;
      END LOOP;
      EXIT WHEN d = 0;
      tup[d] := tup[d] + 1;
    END LOOP;

    -- Conditional on the value existing (and on prov): every k-tuple of the
    -- sum above names a row, so the worlds without one contribute 0 and only
    -- the normalisation is left.
    IF defined_tok <> gate_one() THEN
      IF prov <> gate_one() THEN
        defined_tok := provenance_times(prov, defined_tok);
      END IF;
      prob := probability_evaluate(defined_tok, method, arguments);
      IF prob IS NULL OR prob <= 0 THEN
        RETURN NULL;   -- never defined: SQL NULL
      END IF;
      RETURN total / prob;   -- already conditional; skip the generic norm
    END IF;
  ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
    -- Rank enumeration: per distinct value v, P(MIN = v) is the
    -- probability that some t_i with v_i=v is true and all t_j with
    -- smaller v are false.  For MAX we negate values so the same
    -- "smaller-than" rank logic computes MIN-of-negated, then flip.
    -- The outer multiplier picks up the right sign for the k-th moment
    -- of MAX: E[MAX^k] = (-1)^k * E[MIN(-v)^k], so sign_max = (-1)^k.
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    -- MIN/MAX over the empty input world are NULL (no elements), not ±Infinity:
    -- SQL returns one row with a NULL value.  The moment is therefore CONDITIONAL
    -- on the aggregate being defined (non-empty) -- the empty world is excluded
    -- and the result renormalised by P(prov AND non-empty).  (count, whose empty
    -- value 0 is a real value, keeps the empty world; sum keeps it too, as 0.)
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: MIN/MAX undefined
    END IF;

    -- Numerator E[MIN^k . 1{prov AND non-empty}] (the rank sum naturally omits
    -- the empty world, since every term requires a present token).
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT sign_max * COALESCE(SUM(p * power(v, k)), 0) FROM (
        SELECT t1.v AS v,
          probability_evaluate(
            CASE WHEN prov = gate_one()
                 THEN provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                       provenance_plus(ARRAY_AGG(t2.tok)))
                 ELSE provenance_times(prov,
                        provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                         provenance_plus(ARRAY_AGG(t2.tok)))) END,
            method, arguments) AS p
        FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
        GROUP BY t1.v) tmp
      INTO total;

    -- Denominator P(prov AND non-empty) = P(prov (x) (+) tokens).
    SELECT probability_evaluate(
             CASE WHEN prov = gate_one()
                  THEN provenance_plus(ARRAY_AGG(tok))
                  ELSE provenance_times(prov, provenance_plus(ARRAY_AGG(tok))) END,
             method, arguments)
      FROM (SELECT (get_children(c))[1] AS tok FROM UNNEST(child_pairs) AS c) s
      INTO total_probability;

    IF total_probability <= 0 THEN
      RETURN NULL;  -- never defined under prov: MIN/MAX undefined
    END IF;
    RETURN total / total_probability;  -- already conditional; skip generic norm
  ELSIF aggregation_function = 'avg' THEN
    -- AVG = SUM/COUNT is a ratio of two correlated world-dependent
    -- quantities, so the k-tuple expansion above does not apply.  Like
    -- MIN/MAX, AVG over the empty world is NULL, so its moment conditions
    -- on the aggregate being defined (COUNT >= 1), NULL when it never is.
    -- Two routes:
    --  * EXACT (independent rows, unconditional): the joint (sum, count)
    --    PMF folded in C by agg_avg_moment_exact --
    --    E[AVG^k | COUNT>=1] = Σ_{(s,c), c>=1} (s/c)^k pmf(s,c) / P(c>=1).
    --  * Monte-Carlo scalar fallback otherwise (an outer conditioning
    --    event, shared leaves, compound contributors): rv_moment samples
    --    the agg gate per world; its NaN-skip on empty draws implements
    --    the same conditional-on-defined convention, at the
    --    provsql.rv_mc_samples budget (0 raises, per convention).
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: AVG undefined
    END IF;
    -- Conditioning on an event that AVG being defined implies -- some row
    -- among a set holding AVG's rows present, as provenance() of a GROUP BY
    -- row is, the group possibly having rows whose value is NULL -- is what
    -- the moment already does: the exact route applies.
    IF prov <> gate_one() THEN
      DECLARE
        inner_prov uuid := prov;
        prov_toks uuid[];
        agg_toks uuid[];
      BEGIN
        IF get_gate_type(inner_prov) = 'delta' THEN
          inner_prov := (get_children(inner_prov))[1];
        END IF;
        IF get_gate_type(inner_prov) = 'plus' THEN
          prov_toks := get_children(inner_prov);
        ELSE
          prov_toks := ARRAY[inner_prov];
        END IF;
        SELECT array_agg((get_children(c))[1]) INTO agg_toks
          FROM unnest(child_pairs) AS c;
        IF agg_toks <@ prov_toks THEN
          prov := gate_one();
        END IF;
      END;
    END IF;
    IF prov = gate_one() THEN
      total := agg_avg_moment_exact((token)::uuid, k);
      IF total IS NOT NULL THEN
        RETURN total;
      END IF;
    END IF;
    RETURN rv_moment((token)::uuid, k, false, prov);
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function,
      DETAIL = 'provsql-reason: moment-aggregate-kind; scope: gap';
  END IF;

  -- Conditional normalisation: E[X^k · 1_A] / P(A) = E[X^k | A].
  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

-- ----------------------------------------------------------------------
-- 6n. A division by zero of an aggregate is a NULL value, keeping its gate,
--     rather than an error: the row may not be in the database as it is.
-- ----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION agg_arith_make(op int, children uuid[], val numeric)
  RETURNS agg_token AS
$$
DECLARE
  token uuid := public.uuid_generate_v5(
    provsql.uuid_ns_provsql(), concat('arith', op::text, children::text));
BEGIN
  IF op IS NULL OR children IS NULL THEN
    RETURN NULL;
  END IF;
  PERFORM provsql.create_gate(token, 'arith', children, op, NULL, val::text);
  -- A NULL value (a division by zero on a row the database as it is may not
  -- have) keeps the gate: its value in the other worlds is in the circuit.
  RETURN format('( %s , %s )', token::text,
                COALESCE(val::text, 'NULL'))::provsql.agg_token;
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_token_div(a agg_token, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, (b)::uuid],
     provsql.agg_token_value(a) / NULLIF(provsql.agg_token_value(b), 0)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION agg_token_div_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) / NULLIF(b, 0)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION numeric_div_agg_token(a numeric, b agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(3, ARRAY[provsql.agg_value_gate(a), (b)::uuid],
     a / NULLIF(provsql.agg_token_value(b), 0)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

-- ----------------------------------------------------------------------
-- 6o. array_collect: array_agg with the empty array over no rows, the
--     value of ARRAY(SELECT ...).
-- ----------------------------------------------------------------------

CREATE FUNCTION array_collect_step(state ANYARRAY, data ANYNONARRAY)
  RETURNS ANYARRAY AS
$$ SELECT array_append(state, data) $$
LANGUAGE sql PARALLEL SAFE IMMUTABLE;

CREATE AGGREGATE array_collect(ANYNONARRAY) (
  SFUNC = array_collect_step,
  STYPE = ANYARRAY,
  INITCOND = '{}'
);

-- ----------------------------------------------------------------------
-- 6p. epsilon(): a 0.001 threshold nothing compares a probability with any
--     more.  An event of probability 0.001 is possible, so what it used to
--     decide -- that an aggregate is never defined, that a value is out of
--     the support -- is decided by impossibility (<= 0) instead.  Every
--     function that called it (agg_raw_moment, agg_case, agg_defined_event,
--     support) is redefined above or by an earlier upgrade.
-- ----------------------------------------------------------------------

DROP FUNCTION IF EXISTS epsilon();

-- ----------------------------------------------------------------------
-- 6q. nonzero(token, 'boolean') without a mapping reads the truth of a
--     comparison of aggregate results off the values they record
--     (plain_truth), instead of over the worlds of what they aggregate, which
--     costs one term per subset of the contributions.
-- ----------------------------------------------------------------------

CREATE OR REPLACE FUNCTION nonzero(token uuid,
                        semiring text DEFAULT NULL,
                        mapping regclass DEFAULT NULL)
  RETURNS boolean AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN true;
  END IF;
  IF semiring IS NULL THEN
    RETURN provsql.true_nonzero(token);
  ELSIF semiring = 'boolean' THEN
    IF mapping IS NULL THEN
      RETURN provsql.plain_truth(token);
    END IF;
    RETURN provsql.provenance_evaluate_compiled(token, mapping, 'boolean', TRUE);
  ELSIF semiring = 'counting' THEN
    RETURN provsql.provenance_evaluate_compiled(token, mapping, 'counting', 1) <> 0;
  ELSE
    RAISE EXCEPTION 'nonzero: unsupported semiring "%" (supported: boolean, counting; NULL for the universal zero test)', semiring;
  END IF;
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

-- ----------------------------------------------------------------------
-- 6r. agg_guard_holds: a comparison whose side carries a value but has none
--     in the actual data (an aggregate over no row there) does not hold, as
--     provenance_cmp annotates it zero, rather than leaving the truth
--     undecided -- which sent plain_truth to the reading over the worlds of
--     what the comparison aggregates.
-- ----------------------------------------------------------------------

/** @brief Whether @p token carries a value but records none in the actual
 *  data: an aggregate over no row there, a value gate without a constant.
 *
 *  Told apart from a value this reading does not take -- a timestamp, which
 *  @c agg_gate_value gives up on because it reads numbers, or a random
 *  variable, which has no value in the actual data at all -- because the two
 *  call for opposite answers in @c agg_guard_holds: a comparison with a side
 *  that HAS no value there does not hold, while one whose value is simply not
 *  read leaves the truth undecided (internal use). */
CREATE OR REPLACE FUNCTION agg_gate_value_missing(token uuid)
  RETURNS boolean AS
$$
DECLARE
  gt provsql.provenance_gate := provsql.get_gate_type(token);
  ch uuid[];
BEGIN
  IF gt IN ('agg', 'arith', 'value') THEN
    RETURN provsql.get_extra(token) IS NULL;
  ELSIF gt = 'semimod' THEN
    ch := provsql.get_children(token);
    RETURN array_length(ch, 1) = 2
           AND provsql.get_extra(ch[2]) IS NULL;
  ELSIF gt = 'conditioned' THEN
    ch := provsql.get_children(token);
    RETURN array_length(ch, 1) >= 1
           AND provsql.agg_gate_value_missing(ch[1]);
  END IF;
  /* Any other gate: nothing is claimed */
  RETURN false;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

/**
 * @brief Deterministic truth of a Boolean guard sub-circuit over aggregate
 *        comparisons, evaluated in the actual world (all input tuples present).
 *
 * Guards are the shapes @c having_Expr_to_provenance_cmp mints: @c cmp gates
 * over aggregate-valued children (comparison-operator OID in @c info1),
 * @c times / @c plus combinations (AND / OR, with negation pushed into the
 * comparison operators), and the @c one / @c zero indicators of regular
 * (aggregate-free) conditions.  Uses Kleene three-valued logic: returns
 * @c NULL on any other gate shape, or when an operand's deterministic value
 * cannot be resolved.
 */
CREATE OR REPLACE FUNCTION agg_guard_holds(token UUID)
  RETURNS boolean AS
$$
DECLARE
  gt provenance_gate := get_gate_type(token);
  ch uuid[];
  opname text;
  l numeric;
  r numeric;
  all_true boolean;
  any_true boolean;
  any_null boolean;
BEGIN
  IF gt = 'one' THEN
    RETURN true;
  ELSIF gt = 'zero' THEN
    RETURN false;
  ELSIF gt IN ('times', 'plus') THEN
    SELECT bool_and(h), bool_or(h), bool_or(h IS NULL)
      INTO all_true, any_true, any_null
      FROM (SELECT provsql.agg_guard_holds(c) AS h
            FROM unnest(get_children(token)) AS c) AS s;
    IF gt = 'times' THEN
      -- AND: false dominates unknown (bool_and skips NULL inputs, so it is
      -- false exactly when some child is false).
      RETURN CASE WHEN NOT all_true THEN false
                  WHEN any_null THEN NULL
                  ELSE true END;
    ELSE
      -- OR: true dominates unknown.
      RETURN CASE WHEN any_true THEN true
                  WHEN any_null THEN NULL
                  ELSE false END;
    END IF;
  ELSIF gt = 'cmp' THEN
    ch := get_children(token);
    l := agg_gate_value(ch[1]);
    r := agg_gate_value(ch[2]);
    IF l IS NULL OR r IS NULL THEN
      /* A side with no value in the actual data -- an aggregate over no row
       * there, as a group kept only for other worlds is -- makes the
       * comparison unknown there, which is what provenance_cmp annotates
       * zero: it does not hold.  A side whose value this reading does not
       * take, a timestamp or a random variable, leaves the truth undecided
       * instead: the value is there, only not as a number. */
      IF agg_gate_value_missing(ch[1]) OR agg_gate_value_missing(ch[2]) THEN
        RETURN false;
      END IF;
      RETURN NULL;
    END IF;
    SELECT oprname INTO opname
      FROM pg_catalog.pg_operator WHERE oid = (get_infos(token)).info1;
    RETURN CASE opname
      WHEN '<'  THEN l <  r
      WHEN '<=' THEN l <= r
      WHEN '='  THEN l =  r
      WHEN '<>' THEN l <> r
      WHEN '>=' THEN l >= r
      WHEN '>'  THEN l >  r
    END;
  ELSIF gt IN ('input', 'delta', 'monus', 'project', 'eq', 'mulinput',
               'assumed', 'annotation') THEN
    /* An ordinary provenance expression, not a comparison: the guard a
     * COALESCE over an aggregate lowers to is the NullTest one,
     * delta(+Kn) for IS NOT NULL and 1 - +Kn for IS NULL.  Such a guard
     * holds in the actual data exactly when its Boolean provenance does
     * with every input row present, which is what plain_truth reads. */
    RETURN provsql.plain_truth(token);
  END IF;
  RETURN NULL;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

-- ----------------------------------------------------------------------
-- 6s. ln / exp / sqrt of an aggregate result, and its power of a constant:
--     the gate computes them in every world, where reading them off the value
--     of the database as it is left the result untracked.
-- ----------------------------------------------------------------------

/** @brief ln(agg_token) (gate_arith LN): the logarithm of the value the
 *  aggregate takes, in every world, rather than of the one it takes in the
 *  database as it is. */
CREATE OR REPLACE FUNCTION ln(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(8, ARRAY[(a)::uuid],
     ln(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief exp(agg_token) (gate_arith EXP). */
CREATE OR REPLACE FUNCTION exp(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(9, ARRAY[(a)::uuid],
     exp(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief sqrt(agg_token): the square root is the power of one half
 *  (gate_arith POW, whose exponent is a value gate). */
CREATE OR REPLACE FUNCTION sqrt(a agg_token)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(7,
     ARRAY[(a)::uuid, provsql.agg_value_gate(0.5)],
     sqrt(provsql.agg_token_value(a))); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

/** @brief agg_token ^ numeric (gate_arith POW, constant lifted to a value
 *  gate). */
CREATE OR REPLACE FUNCTION agg_token_pow_numeric(a agg_token, b numeric)
  RETURNS agg_token AS
$$ SELECT provsql.agg_arith_make(7,
     ARRAY[(a)::uuid, provsql.agg_value_gate(b)],
     provsql.agg_token_value(a) ^ b); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OPERATOR ^ (LEFTARG=agg_token, RIGHTARG=numeric,
                   PROCEDURE=agg_token_pow_numeric);

-- ----------------------------------------------------------------------
-- 6t. agg_gate_value reads the text of a value without a PL/pgSQL exception
--     block: entering one starts a subtransaction, which a parallel worker
--     cannot do, and the evaluator calls this function wherever the query
--     runs.
-- ----------------------------------------------------------------------

/**
 * @brief Deterministic (actual-world) scalar value of an aggregate-carrying
 *        gate.
 *
 * Resolves the value an aggregate expression takes on the actual data -- the
 * value an @c agg_token display cell carries: @c agg / @c arith gates record
 * it in @c extra (set by aggregate evaluation and @c agg_arith_make), a
 * @c value gate carries its constant, a @c semimod wraps a value gate, a
 * @c conditioned gate has its target's value, and a @c case gate selects the
 * first branch whose guard holds in the actual world (per
 * @c agg_guard_holds), else the default.  Returns @c NULL when the gate is
 * not aggregate-carrying or the value cannot be resolved (e.g. a
 * non-numeric aggregate).
 */
CREATE OR REPLACE FUNCTION agg_gate_value(token UUID)
  RETURNS numeric AS
$$
DECLARE
  gt provenance_gate := get_gate_type(token);
  ch uuid[];
  n integer;
  holds boolean;
  extra text;
BEGIN
  IF gt IN ('agg', 'arith', 'value') THEN
    /* Reading the text as a number without a PL/pgSQL exception block, which
     * a parallel worker cannot afford: entering one starts a subtransaction,
     * and this function is called from the evaluator, which runs wherever the
     * query does (PostgreSQL 11 raises "cannot start subtransactions during a
     * parallel operation").  What is not the text of a number is the value of
     * a non-numeric aggregate (a min over text, a timestamp), which this
     * reading does not take. */
    extra := get_extra(token);
    IF extra ~ '^\s*([-+]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?|[Nn][Aa][Nn])\s*$' THEN
      RETURN extra::numeric;
    END IF;
    RETURN NULL;
  ELSIF gt = 'semimod' THEN
    RETURN agg_gate_value((get_children(token))[2]);
  ELSIF gt = 'conditioned' THEN
    RETURN agg_gate_value((get_children(token))[1]);
  ELSIF gt = 'case' THEN
    ch := get_children(token);
    n := array_length(ch, 1);
    FOR i IN 1 .. (n - 1) / 2 LOOP
      holds := agg_guard_holds(ch[2 * i - 1]);
      IF holds IS NULL THEN
        RETURN NULL;
      ELSIF holds THEN
        RETURN agg_gate_value(ch[2 * i]);
      END IF;
    END LOOP;
    RETURN agg_gate_value(ch[n]);
  END IF;
  RETURN NULL;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

-- ----------------------------------------------------------------------
-- 6u. eval_recursive_all: the driver of a UNION ALL recursion, whose rounds
--     read the previous round and whose answer is their bag union, ending on
--     a round that derives nothing.
-- ----------------------------------------------------------------------

/**
 * @brief Drive a @c UNION @c ALL recursion, one round per bag of derivations
 *
 * The bag recursion is not a fixpoint over a set: its rounds are
 * @c M0 @c = @c q0 and @c M(i+1) @c = @c q1 over @c Mi -- the PREVIOUS round,
 * not what has been derived so far -- and its answer is the bag union of every
 * round, which ends when a round derives nothing.  Each tuple of it is one
 * derivation, annotated by the product along that derivation, and two
 * derivations of the same tuple are two rows and are not merged: that is what
 * distinguishes it from @c UNION, whose driver (@c eval_recursive) sums the
 * derivations of a tuple into one row and stops when the set of rows stops
 * changing.
 *
 * @c work_name holds the previous round, which the recursive term reads by the
 * name of the CTE; @c all_name accumulates the answer and is what the query
 * reads.  Ending on an empty round is SQL's own rule, so a recursion PostgreSQL
 * runs to completion ends here too, and one it does not is caught by
 * @p max_iter as before.
 *
 * @param q0_sql     the non-recursive term, as SQL
 * @param q1_sql     the recursive term, reading @p work_name
 * @param work_name  temp table of the previous round (the CTE's name)
 * @param all_name   temp table accumulating the rounds
 * @param colnames   comma-separated user column names
 * @param coldef     column definitions ("name type, ...")
 * @param max_iter   safety bound on the number of rounds
 */
CREATE OR REPLACE FUNCTION eval_recursive_all(
  q0_sql    text,
  q1_sql    text,
  work_name text,
  all_name  text,
  colnames  text,
  coldef    text,
  max_iter  int DEFAULT 1000)
  RETURNS void AS
$$
DECLARE
  iters     int := 0;
  new_count int;
BEGIN
  EXECUTE format('DROP TABLE IF EXISTS %I', work_name);
  EXECUTE format('DROP TABLE IF EXISTS %I', all_name);
  DROP TABLE IF EXISTS provsql_rec_new;

  EXECUTE format('CREATE TEMP TABLE %I (%s, provsql uuid) ON COMMIT DROP',
                 work_name, coldef);
  EXECUTE format('CREATE TEMP TABLE %I (LIKE %I) ON COMMIT DROP',
                 all_name, work_name);
  EXECUTE format('CREATE TEMP TABLE provsql_rec_new (LIKE %I) ON COMMIT DROP',
                 work_name);
  PERFORM provsql.planted_scope(all_name);

  -- The first round: the non-recursive term.
  EXECUTE format('INSERT INTO provsql_rec_new(%s) %s', colnames, q0_sql);
  GET DIAGNOSTICS new_count = ROW_COUNT;

  LOOP
    EXIT WHEN new_count = 0;   -- a round that derives nothing ends the answer

    -- Every row of the round is an answer, kept as it is: a second derivation
    -- of a tuple is a second row, which is what UNION ALL says.
    EXECUTE format('INSERT INTO %1$I(%2$s) SELECT %2$s FROM provsql_rec_new',
                   all_name, colnames);

    -- The round becomes the relation the recursive term reads.
    EXECUTE format('TRUNCATE %I', work_name);
    EXECUTE format('INSERT INTO %1$I(%2$s) SELECT %2$s FROM provsql_rec_new',
                   work_name, colnames);

    iters := iters + 1;
    IF iters > max_iter THEN
      /* A bag recursion is defined when a round derives nothing; one whose
       * rounds do not end has no answer to give, in SQL either (PostgreSQL
       * runs it forever), so this is what the recursion means and not a limit
       * of the driver. */
      RAISE EXCEPTION 'ProvSQL: the rounds of this UNION ALL recursion do not '
                      'end (after % of them): its answer is the rows of every '
                      'round, which SQL itself does not reach either on such '
                      'data', max_iter
        USING ERRCODE = 'feature_not_supported',
              DETAIL = 'provsql-reason: recursion-does-not-end; scope: deliberate';
    END IF;

    EXECUTE format('TRUNCATE provsql_rec_new');
    EXECUTE format('INSERT INTO provsql_rec_new(%s) %s', colnames, q1_sql);
    GET DIAGNOSTICS new_count = ROW_COUNT;
  END LOOP;
END
$$ LANGUAGE plpgsql SET client_min_messages = warning;

-- ----------------------------------------------------------------------
-- 7. The C side caches the OID of each enum value per session; a backend
--    warmed under the previous version would not know the two values
--    added in section 1.
-- ----------------------------------------------------------------------

SELECT reset_constants_cache();
