/**
 * @file circuit_cleanup.c
 * @brief Removing from the circuit store what nothing references any more.
 *
 * The store only grows.  A gate is never removed, and that is deliberate:
 * gates are immutable and content-addressed, so a transaction that rolls
 * back leaves an orphan rather than an inconsistency, and the same
 * expression recomputed lands on the same gate.  The price is that every
 * dropped table, every @c remove_provenance, every reloaded dataset and
 * every exploratory join over a large table leaves gates behind for good.
 *
 * @c provsql.circuit_cleanup() is the complement of that: the one
 * operation allowed to remove gates, run explicitly, the way
 * @c VACUUM @c FULL is the complement of MVCC.  It keeps every gate
 * reachable from a token stored in the database and discards the rest,
 * rewriting the four store files compactly on the way -- which also makes
 * it the repair tool for a store damaged by an interrupted write (see
 * @c provsql.check_store).
 *
 * **Why it needs the database to itself.**  Two properties rule out a
 * concurrent collector.  Gates are re-created idempotently: a query
 * running alongside can compute @c provenance_plus(a, b), find the
 * content-addressed gate already there as an orphan, and reference it a
 * moment before the collector -- which saw no reference -- removes it.
 * And a transaction in progress has created gates and inserted rows that
 * the collector's snapshot cannot see.  So the function takes the
 * database's lock in the mode @c DROP @c DATABASE takes it, and refuses
 * to run while another session is connected, exactly as @c DROP
 * @c DATABASE does.
 *
 * **What counts as a root.**  Every value of a @c uuid, @c agg_token or
 * @c random_variable column -- and of arrays of those -- in every table
 * and materialised view of the database, whatever the column is called:
 * @c "CREATE TABLE ... AS SELECT provenance()" stores tokens under
 * whatever name the user picks, mapping tables use @c provenance,
 * @c update_provenance uses @c provsql, and users store @c get_children
 * results and conditioning events wherever they like.  Scanning by type
 * is the only conservative choice, because a missed root turns a derived
 * gate back into an input, silently.  The semiring constants @c gate_zero
 * and @c gate_one are roots unconditionally: the planner emits their
 * UUIDs as literals, so nothing in the database need mention them.
 *
 * A token that lives only *outside* the database -- copied into a
 * notebook cell or a deep link, kept in a file, stored as text or inside
 * a @c jsonb document -- is not a root.  Content-addressed gates come
 * back by re-running the query that produced them; freshly minted ones
 * (an @c rv leaf, the @c update gate of a deleted log row, the input gate
 * of a row deleted from an untracked copy) do not.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/uuid.h"

#include "circuit_cache.h"
#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

/** @brief Growable array of root tokens gathered from the database. */
typedef struct root_set {
  pg_uuid_t *tokens;
  int64      len;
  int64      cap;
} root_set;

static void root_set_add(root_set *rs, const pg_uuid_t *token)
{
  if(rs->len == rs->cap) {
    int64 newcap = rs->cap ? rs->cap * 2 : 1024;
    pg_uuid_t *grown = repalloc(rs->tokens, newcap * sizeof(pg_uuid_t));
    rs->tokens = grown;
    rs->cap = newcap;
  }
  rs->tokens[rs->len++] = *token;
}

/**
 * @brief Run @p query and append every UUID it returns to @p rs.
 *
 * The query is expected to return one @c uuid column.
 */
static void collect_from(root_set *rs, const char *query)
{
  int rc = SPI_execute(query, true, 0);
  if(rc != SPI_OK_SELECT)
    provsql_error("circuit_cleanup: cannot collect roots (SPI code %d)", rc);

  for(uint64 i = 0; i < SPI_processed; ++i) {
    bool isnull;
    Datum d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc,
                            1, &isnull);
    if(!isnull)
      root_set_add(rs, DatumGetUUIDP(d));
  }
}

/**
 * @brief Gather every token stored in the database.
 *
 * Walks @c pg_attribute for columns whose type is one ProvSQL tokens live
 * in, then reads each of them.  Provenance tracking is switched off for
 * the duration: these reads would otherwise materialise an input gate per
 * row of every tracked table, which is exactly the growth the clean-up is
 * there to undo.
 */
static void collect_roots(root_set *rs)
{
  int save_nestlevel = NewGUCNestLevel();
  int rc;

  SetConfigOption("provsql.active", "off",
                  PGC_USERSET, PGC_S_SESSION);

  PG_TRY();
  {
    /* The semiring constants: the planner emits their UUIDs as literals,
       so no row need mention them, and a store that lost them would read
       them back as inputs rather than as constants. */
    collect_from(rs, "SELECT provsql.gate_zero()");
    collect_from(rs, "SELECT provsql.gate_one()");

    rc = SPI_execute(
      "SELECT c.oid::regclass::text AS rel, quote_ident(a.attname) AS col, "
      "       t.typelem <> 0 AND t.typlen = -1 AS is_array "
      "  FROM pg_attribute a "
      "  JOIN pg_class c ON c.oid = a.attrelid "
      "  JOIN pg_namespace n ON n.oid = c.relnamespace "
      "  JOIN pg_type t ON t.oid = a.atttypid "
      " WHERE c.relkind IN ('r', 'm', 'p') "
      "   AND a.attnum > 0 AND NOT a.attisdropped "
      "   AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast') "
      "   AND (CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN t.typelem "
      "             ELSE t.oid END) IN ("
      "         'uuid'::regtype, 'provsql.agg_token'::regtype, "
      "         'provsql.random_variable'::regtype) "
      " ORDER BY 1, 2", true, 0);
    if(rc != SPI_OK_SELECT)
      provsql_error("circuit_cleanup: cannot enumerate token columns "
                    "(SPI code %d)", rc);

    {
      uint64 n = SPI_processed;
      char **rels = palloc(n * sizeof(char *));
      char **cols = palloc(n * sizeof(char *));
      bool  *arrs = palloc(n * sizeof(bool));

      for(uint64 i = 0; i < n; ++i) {
        rels[i] = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
        cols[i] = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);
        arrs[i] = (strcmp(SPI_getvalue(SPI_tuptable->vals[i],
                                       SPI_tuptable->tupdesc, 3), "t") == 0);
      }

      for(uint64 i = 0; i < n; ++i) {
        StringInfoData buf;
        initStringInfo(&buf);
        if(arrs[i])
          appendStringInfo(&buf,
                           "SELECT DISTINCT u::uuid FROM %s, "
                           "LATERAL unnest(%s) AS u WHERE u IS NOT NULL",
                           rels[i], cols[i]);
        else
          appendStringInfo(&buf,
                           "SELECT DISTINCT %s::uuid FROM %s WHERE %s IS NOT NULL",
                           cols[i], rels[i], cols[i]);
        collect_from(rs, buf.data);
        pfree(buf.data);
      }
    }

    /* A foreign table can hold tokens too, but scanning one means talking
       to another server -- which may be down, slow, or not there at all.
       Say so rather than silently treating its rows as absent. */
    rc = SPI_execute(
      "SELECT count(*) FROM pg_attribute a "
      "  JOIN pg_class c ON c.oid = a.attrelid "
      "  JOIN pg_type t ON t.oid = a.atttypid "
      " WHERE c.relkind = 'f' AND a.attnum > 0 AND NOT a.attisdropped "
      "   AND (CASE WHEN t.typelem <> 0 AND t.typlen = -1 THEN t.typelem "
      "             ELSE t.oid END) IN ("
      "         'uuid'::regtype, 'provsql.agg_token'::regtype, "
      "         'provsql.random_variable'::regtype)", true, 0);
    if(rc == SPI_OK_SELECT && SPI_processed == 1) {
      char *cnt = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
      if(cnt && strcmp(cnt, "0") != 0)
        provsql_notice("circuit_cleanup: %s token-typed column(s) live on "
                       "foreign tables and were not scanned; gates only they "
                       "reference are removed", cnt);
    }
  }
  PG_CATCH();
  {
    AtEOXact_GUC(false, save_nestlevel);
    PG_RE_THROW();
  }
  PG_END_TRY();

  AtEOXact_GUC(false, save_nestlevel);
}

PG_FUNCTION_INFO_V1(circuit_cleanup);
/**
 * @brief Rebuild this database's circuit store, keeping only what the
 *        tokens stored in the database reach.
 *
 * Takes the database exclusively for the duration; see the file comment
 * for why that is unavoidable.  @p dry_run reports how much would be kept
 * without writing anything.
 */
Datum circuit_cleanup(PG_FUNCTION_ARGS)
{
  bool dry_run = PG_ARGISNULL(0) ? false : PG_GETARG_BOOL(0);
  root_set rs = { NULL, 0, 0 };
  provsql_cleanup_result res;
  int nprepared = 0;
  TupleDesc tupdesc;
  Datum values[6];
  bool nulls[6] = {false, false, false, false, false, false};

  if(RecoveryInProgress())
    ereport(ERROR,
            (errmsg("provsql.circuit_cleanup() cannot run on a standby"),
             errdetail("The circuit store is not replicated, and a standby "
                       "must not write to it.")));

  if(provsql_store_written())
    ereport(ERROR,
            (errmsg("provsql.circuit_cleanup() cannot run in a transaction "
                    "that has already written to the circuit store"),
             errhint("Run it as the first statement of its own transaction.")));

  /* The lock DROP DATABASE takes: every new connection takes it in a
     weaker mode during its startup transaction, so sessions that arrive
     from now on wait for us. */
  LockSharedObject(DatabaseRelationId, MyDatabaseId, 0, AccessExclusiveLock);

  /* ... and the check DROP DATABASE makes, for the sessions that are
     already here.  CountOtherDBBackends terminates autovacuum workers and
     waits for them; anything else is the operator's to disconnect. */
  {
    int nbackends = 0;
    if(CountOtherDBBackends(MyDatabaseId, &nbackends, &nprepared))
      ereport(ERROR,
              (errcode(ERRCODE_OBJECT_IN_USE),
               errmsg("database \"%s\" is being accessed by other users",
                      get_database_name(MyDatabaseId)),
               errdetail("There %s %d other session%s and %d prepared "
                         "transaction%s using the database.",
                         nbackends == 1 ? "is" : "are", nbackends,
                         nbackends == 1 ? "" : "s", nprepared,
                         nprepared == 1 ? "" : "s")));
  }

  /* Our own caches answer "this gate exists" without contacting the
     worker, which would survive the rebuild as a lie. */
  circuit_cache_reset();

  /* The root set outlives the SPI session that fills it, so it is
     allocated in our own context: SPI_finish frees everything palloc'd
     while connected. */
  rs.tokens = MemoryContextAlloc(CurrentMemoryContext,
                                 1024 * sizeof(pg_uuid_t));
  rs.cap = 1024;

  if(SPI_connect() != SPI_OK_CONNECT)
    provsql_error("circuit_cleanup: cannot connect to SPI");
  collect_roots(&rs);
  SPI_finish();

  provsql_circuit_cleanup_request(dry_run, rs.tokens, rs.len, &res);

  if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    provsql_error("circuit_cleanup: expected composite return type");
  tupdesc = BlessTupleDesc(tupdesc);

  values[0] = Int64GetDatum((int64) res.gates_before);
  values[1] = Int64GetDatum((int64) res.gates_after);
  values[2] = Int64GetDatum((int64) res.wires_before);
  values[3] = Int64GetDatum((int64) res.wires_after);
  values[4] = Int64GetDatum((int64) res.extra_before);
  values[5] = Int64GetDatum((int64) res.extra_after);
  if(dry_run) {
    /* A dry run measures the mark phase only: the wire and byte totals of
       a rewrite are not known without doing it. */
    nulls[3] = true;
    nulls[5] = true;
  }

  PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
