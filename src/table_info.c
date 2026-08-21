/**
 * @file table_info.c
 * @brief Heap-backed storage for per-relation provenance metadata.
 *
 * The @c provsql.table_info table holds one row per relation ProvSQL
 * tracks: its TID / BID / OPAQUE classification, the block-key columns
 * of a BID relation, and the base relations its atoms come from.  This
 * file implements the SQL entry points that read and write it
 * (@c set_table_info, @c remove_table_info, @c get_table_info,
 * @c set_ancestors, @c remove_ancestors, @c get_ancestors) and the
 * uncached C fetchers behind the planner-hot-path caches in
 * @c provsql_utils.c.
 *
 * Metadata about relations is catalog-shaped data, so the heap is its
 * natural home: every change follows the transaction that made it, a
 * concurrent session sees it only once it commits, and @c pg_dump
 * carries it (the table is registered with
 * @c pg_extension_config_dump).  The circuit store proper holds only
 * the circuit.
 *
 * Reads go through @c systable_beginscan rather than SPI: the planner
 * hook consults them for every provenance-tracked range-table entry,
 * and running a full query through the planner from inside the planner
 * hook is both slow and needlessly re-entrant.  Writes, which happen
 * once per @c add_provenance / @c repair_key / guard-trigger fire, use
 * SPI for brevity.
 *
 * The @c provsql_table_info_invalidate row trigger on the table
 * broadcasts a relcache invalidation for each changed relation, which
 * is what drops the stale entry from every backend's cache -- including
 * after a direct @c UPDATE on the table or a @c pg_restore that loads
 * it with @c COPY.
 */
#include "postgres.h"

#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"               /* table_open / table_close */
#else
#include "access/heapam.h"              /* heap_open / heap_close (PG <12) */
#define table_open(r, l)  heap_open((r), (l))
#define table_close(r, l) heap_close((r), (l))
#endif
#include "access/genam.h"
#include "access/skey.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "provsql_utils.h"
#include "MMappedTableInfo.h"

/** @brief Column numbers of @c provsql.table_info. */
#define PROVSQL_TABLE_INFO_ATT_RELID     1
#define PROVSQL_TABLE_INFO_ATT_KIND      2
#define PROVSQL_TABLE_INFO_ATT_BLOCK_KEY 3
#define PROVSQL_TABLE_INFO_ATT_ANCESTORS 4

/**
 * @brief Resolve @c provsql.table_info and its primary-key index.
 *
 * Returns @c InvalidOid (and leaves @p *index_oid untouched) when the
 * table does not exist -- which is the normal state while
 * @c CREATE @c EXTENSION runs, and on an installation still on an
 * extension version that predates it.  Every caller then behaves as
 * "no metadata recorded", the conservative direction.
 */
static Oid provsql_table_info_relation(Oid *index_oid)
{
  Oid nsp = get_namespace_oid("provsql", true);
  Oid rel;

  if(!OidIsValid(nsp))
    return InvalidOid;
  rel = get_relname_relid("table_info", nsp);
  if(!OidIsValid(rel))
    return InvalidOid;
  if(index_oid)
    *index_oid = get_relname_relid("table_info_pkey", nsp);
  return rel;
}

/**
 * @brief Read the row of @p relid into @p out.
 *
 * @return @c true when a row exists; @p out is then fully populated
 *         (both the kind and the ancestor halves).
 */
static bool provsql_read_table_info(Oid relid, ProvenanceTableInfo *out)
{
  Oid          rel_oid, index_oid = InvalidOid;
  Relation     rel;
  SysScanDesc  scan;
  ScanKeyData  skey;
  HeapTuple    htup;
  bool         found = false;

  if(relid == InvalidOid)
    return false;

  rel_oid = provsql_table_info_relation(&index_oid);
  if(!OidIsValid(rel_oid))
    return false;

  memset(out, 0, sizeof(*out));
  out->relid = relid;
  out->kind  = PROVSQL_TABLE_OPAQUE;

  rel = table_open(rel_oid, AccessShareLock);
  ScanKeyInit(&skey, PROVSQL_TABLE_INFO_ATT_RELID,
              BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
  scan = systable_beginscan(rel, index_oid, OidIsValid(index_oid),
                            NULL, 1, &skey);

  if(HeapTupleIsValid(htup = systable_getnext(scan))) {
    TupleDesc tupdesc = RelationGetDescr(rel);
    bool      isnull;
    Datum     d;

    found = true;

    d = heap_getattr(htup, PROVSQL_TABLE_INFO_ATT_KIND, tupdesc, &isnull);
    if(!isnull) {
      char *kind = text_to_cstring(DatumGetTextPP(d));
      if(strcmp(kind, "tid") == 0)
        out->kind = PROVSQL_TABLE_TID;
      else if(strcmp(kind, "bid") == 0)
        out->kind = PROVSQL_TABLE_BID;
      else
        out->kind = PROVSQL_TABLE_OPAQUE;
      pfree(kind);
    }

    d = heap_getattr(htup, PROVSQL_TABLE_INFO_ATT_BLOCK_KEY, tupdesc, &isnull);
    if(!isnull) {
      ArrayType *arr = DatumGetArrayTypeP(d);
      if(ARR_NDIM(arr) == 1 && !array_contains_nulls(arr)) {
        int n = ARR_DIMS(arr)[0];
        if(n > PROVSQL_TABLE_INFO_MAX_BLOCK_KEY)
          n = PROVSQL_TABLE_INFO_MAX_BLOCK_KEY;
        out->block_key_n = (uint16) n;
        memcpy(out->block_key, ARR_DATA_PTR(arr), n * sizeof(AttrNumber));
      }
    }

    d = heap_getattr(htup, PROVSQL_TABLE_INFO_ATT_ANCESTORS, tupdesc, &isnull);
    if(!isnull) {
      ArrayType *arr = DatumGetArrayTypeP(d);
      if(ARR_NDIM(arr) == 1 && !array_contains_nulls(arr)) {
        int n = ARR_DIMS(arr)[0];
        if(n > PROVSQL_TABLE_INFO_MAX_ANCESTORS)
          n = PROVSQL_TABLE_INFO_MAX_ANCESTORS;
        out->ancestor_n = (uint16) n;
        memcpy(out->ancestors, ARR_DATA_PTR(arr), n * sizeof(Oid));
      }
    }
  }

  systable_endscan(scan);
  table_close(rel, AccessShareLock);

  return found;
}

bool provsql_fetch_table_info(Oid relid, ProvenanceTableInfo *out)
{
  return provsql_read_table_info(relid, out);
}

bool provsql_fetch_ancestry(Oid relid, uint16 *ancestor_n_out,
                            Oid *ancestors_out)
{
  ProvenanceTableInfo info;

  *ancestor_n_out = 0;
  if(!provsql_read_table_info(relid, &info))
    return false;
  *ancestor_n_out = info.ancestor_n;
  memcpy(ancestors_out, info.ancestors, info.ancestor_n * sizeof(Oid));
  /* "Row present but no ancestors" collapses to the same return as
   * "no row": both make the safe-query rewriter take the conservative
   * path. */
  return info.ancestor_n > 0;
}

/** @brief Run @p sql with @p nargs bound parameters through SPI. */
static void provsql_table_info_exec(const char *sql, int nargs,
                                    Oid *argtypes, Datum *values)
{
  int rc;

  if(SPI_connect() != SPI_OK_CONNECT)
    provsql_error("Cannot connect to SPI while updating provsql.table_info");
  rc = SPI_execute_with_args(sql, nargs, argtypes, values, NULL, false, 0);
  if(rc != SPI_OK_INSERT && rc != SPI_OK_UPDATE && rc != SPI_OK_DELETE)
    provsql_error("Cannot update provsql.table_info (SPI code %d)", rc);
  SPI_finish();
}

/** @brief Translate a SQL-side kind label into the persisted enum value. */
static uint8_t parse_table_kind(const char *label)
{
  if(strcmp(label, "tid") == 0) return PROVSQL_TABLE_TID;
  if(strcmp(label, "bid") == 0) return PROVSQL_TABLE_BID;
  if(strcmp(label, "opaque") == 0) return PROVSQL_TABLE_OPAQUE;
  provsql_error("set_table_info: unknown table kind '%s' (expected "
                "'tid', 'bid', or 'opaque')", label);
  return PROVSQL_TABLE_TID;  /* unreachable */
}

/** @brief Inverse of @c parse_table_kind for use by @c get_table_info. */
static const char *table_kind_label(uint8_t kind)
{
  switch(kind) {
  case PROVSQL_TABLE_TID:    return "tid";
  case PROVSQL_TABLE_BID:    return "bid";
  case PROVSQL_TABLE_OPAQUE: return "opaque";
  }
  provsql_error("get_table_info: unknown table kind value %u", kind);
  return NULL;  /* unreachable */
}

PG_FUNCTION_INFO_V1(set_table_info);
/**
 * @brief Upsert the kind half of a relation's @c provsql.table_info row.
 *
 * @p relid is the @c pg_class OID of the relation; @p kind is one of
 * the textual labels @c 'tid' / @c 'bid' / @c 'opaque' (see
 * @c provsql_table_kind in @c MMappedTableInfo.h); @p block_key is an
 * @c int2 array (possibly empty) listing the block-key column numbers
 * when @p kind is @c 'bid'.  The relation's existing @c ancestors are
 * preserved.
 */
Datum set_table_info(PG_FUNCTION_ARGS)
{
  Oid       relid;
  char     *kind_str;
  ArrayType *block_key;
  uint16    block_key_n = 0;
  Oid       argtypes[3] = { OIDOID, TEXTOID, INT2ARRAYOID };
  Datum     values[3];

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("Invalid NULL value passed to set_table_info");

  relid    = PG_GETARG_OID(0);
  kind_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
  (void) parse_table_kind(kind_str);   /* validate, raising on a typo */

  block_key = PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2);
  if(block_key) {
    if(ARR_NDIM(block_key) > 1)
      provsql_error("Invalid multi-dimensional array passed to set_table_info");
    else if(ARR_NDIM(block_key) == 1)
      block_key_n = *ARR_DIMS(block_key);
  }
  if(block_key_n > PROVSQL_TABLE_INFO_MAX_BLOCK_KEY)
    provsql_error("set_table_info: block key wider than %d columns "
                  "(%u given) is not supported",
                  PROVSQL_TABLE_INFO_MAX_BLOCK_KEY, block_key_n);

  values[0] = ObjectIdGetDatum(relid);
  values[1] = CStringGetTextDatum(kind_str);
  values[2] = block_key ? PointerGetDatum(block_key)
              : PointerGetDatum(construct_empty_array(INT2OID));

  provsql_table_info_exec(
    "INSERT INTO provsql.table_info(relid, kind, block_key) "
    "VALUES ($1::regclass, $2, $3) "
    "ON CONFLICT (relid) DO UPDATE "
    "SET kind = EXCLUDED.kind, block_key = EXCLUDED.block_key",
    3, argtypes, values);

  pfree(kind_str);
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(remove_table_info);
/** @brief Delete a relation's @c provsql.table_info row.  No-op when absent. */
Datum remove_table_info(PG_FUNCTION_ARGS)
{
  Oid   argtypes[1] = { OIDOID };
  Datum values[1];

  if(PG_ARGISNULL(0))
    provsql_error("Invalid NULL value passed to remove_table_info");

  values[0] = ObjectIdGetDatum(PG_GETARG_OID(0));
  provsql_table_info_exec(
    "DELETE FROM provsql.table_info WHERE relid = $1::regclass",
    1, argtypes, values);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_ancestors);
/**
 * @brief Replace the ancestor half of a relation's row, keeping its
 *        @c kind / @c block_key.
 *
 * Silently no-op when @p relid has no row yet: the safe-query rewriter
 * only consults ancestry for tracked relations, so callers should run
 * @c add_provenance / @c repair_key / @c set_table_info first.
 */
Datum set_ancestors(PG_FUNCTION_ARGS)
{
  Oid       relid;
  ArrayType *ancestors;
  uint16    ancestor_n = 0;
  Oid       argtypes[2] = { OIDOID, OIDARRAYOID };
  Datum     values[2];

  if(PG_ARGISNULL(0))
    provsql_error("Invalid NULL value passed to set_ancestors");

  relid     = PG_GETARG_OID(0);
  ancestors = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);

  if(ancestors) {
    if(ARR_NDIM(ancestors) > 1)
      provsql_error("Invalid multi-dimensional array passed to set_ancestors");
    else if(ARR_NDIM(ancestors) == 1)
      ancestor_n = *ARR_DIMS(ancestors);
  }
  if(ancestor_n > PROVSQL_TABLE_INFO_MAX_ANCESTORS)
    provsql_error("set_ancestors: ancestor set wider than %d entries "
                  "(%u given) is not supported",
                  PROVSQL_TABLE_INFO_MAX_ANCESTORS, ancestor_n);

  values[0] = ObjectIdGetDatum(relid);
  values[1] = ancestors ? PointerGetDatum(ancestors)
              : PointerGetDatum(construct_empty_array(OIDOID));

  provsql_table_info_exec(
    "UPDATE provsql.table_info SET ancestors = $2 WHERE relid = $1::regclass",
    2, argtypes, values);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(remove_ancestors);
/** @brief Clear a relation's ancestor set, keeping @c kind / @c block_key. */
Datum remove_ancestors(PG_FUNCTION_ARGS)
{
  Oid   argtypes[1] = { OIDOID };
  Datum values[1];

  if(PG_ARGISNULL(0))
    provsql_error("Invalid NULL value passed to remove_ancestors");

  values[0] = ObjectIdGetDatum(PG_GETARG_OID(0));
  provsql_table_info_exec(
    "UPDATE provsql.table_info SET ancestors = ARRAY[]::oid[] "
    "WHERE relid = $1::regclass",
    1, argtypes, values);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_table_info);
/**
 * @brief PostgreSQL-callable wrapper around the cached kind lookup.
 *
 * Returns @c NULL when no row exists for @p relid; otherwise a record
 * @c (kind text, block_key int2[]).  Goes through
 * @c provsql_lookup_table_info so repeated calls in the same session
 * do not re-scan the table.
 */
Datum get_table_info(PG_FUNCTION_ARGS)
{
  Oid relid;
  ProvenanceTableInfo info;
  TupleDesc tupdesc;
  Datum values[2];
  bool nulls[2] = {false, false};
  Datum *elems;
  ArrayType *arr;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  relid = PG_GETARG_OID(0);

  if(!provsql_lookup_table_info(relid, &info))
    PG_RETURN_NULL();

  if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    provsql_error("get_table_info: expected composite return type");
  tupdesc = BlessTupleDesc(tupdesc);

  values[0] = CStringGetTextDatum(table_kind_label(info.kind));

  elems = palloc(info.block_key_n * sizeof(Datum));
  for(uint16 i = 0; i < info.block_key_n; ++i)
    elems[i] = Int16GetDatum(info.block_key[i]);
  arr = construct_array(elems, info.block_key_n, INT2OID, 2, true, 's');
  pfree(elems);
  values[1] = PointerGetDatum(arr);

  PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(get_ancestors);
/**
 * @brief PostgreSQL-callable wrapper around the cached ancestry lookup.
 *
 * Returns @c NULL when no row exists for @p relid, or its ancestor set
 * is empty; otherwise an @c oid[] listing the base-relation OIDs.
 */
Datum get_ancestors(PG_FUNCTION_ARGS)
{
  Oid relid;
  uint16 ancestor_n;
  Oid ancestors[PROVSQL_TABLE_INFO_MAX_ANCESTORS];
  Datum *elems;
  ArrayType *arr;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  relid = PG_GETARG_OID(0);

  if(!provsql_lookup_ancestry(relid, &ancestor_n, ancestors))
    PG_RETURN_NULL();

  elems = palloc(ancestor_n * sizeof(Datum));
  for(uint16 i = 0; i < ancestor_n; ++i)
    elems[i] = ObjectIdGetDatum(ancestors[i]);
  arr = construct_array(elems, ancestor_n, OIDOID,
                        sizeof(Oid), true, 'i');
  pfree(elems);

  PG_RETURN_ARRAYTYPE_P(arr);
}

PG_FUNCTION_INFO_V1(provsql_table_info_invalidate);
/**
 * @brief Row trigger on @c provsql.table_info: broadcast a relcache
 *        invalidation for the relation whose metadata changed.
 *
 * Each backend caches the metadata of the relations its queries touch
 * (@c provsql_lookup_table_info / @c provsql_lookup_ancestry) and drops
 * an entry when PostgreSQL invalidates that relation's relcache entry.
 * Putting the broadcast in a trigger rather than in the setters covers
 * every writer: the setters, a hand-written @c UPDATE on the table, and
 * the @c COPY a @c pg_restore performs.
 *
 * The @c pg_class probe skips relations that are already gone, which is
 * the normal case for the @c DELETE the @c sql_drop event trigger
 * performs.
 */
Datum provsql_table_info_invalidate(PG_FUNCTION_ARGS)
{
  TriggerData *trigdata = (TriggerData *) fcinfo->context;
  TupleDesc    tupdesc;
  HeapTuple    tuples[2];
  int          n = 0;

  if(!CALLED_AS_TRIGGER(fcinfo))
    provsql_error("provsql_table_info_invalidate: not called as a trigger");

  tupdesc = trigdata->tg_relation->rd_att;

  if(TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
    tuples[n++] = trigdata->tg_trigtuple;
  else if(TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
    tuples[n++] = trigdata->tg_trigtuple;
  else {
    /* UPDATE: the row may have been re-keyed, so invalidate both. */
    tuples[n++] = trigdata->tg_trigtuple;
    tuples[n++] = trigdata->tg_newtuple;
  }

  for(int i = 0; i < n; ++i) {
    bool  isnull;
    Datum d;
    Oid   relid;

    if(!HeapTupleIsValid(tuples[i]))
      continue;
    d = heap_getattr(tuples[i], PROVSQL_TABLE_INFO_ATT_RELID, tupdesc, &isnull);
    if(isnull)
      continue;
    relid = DatumGetObjectId(d);
    if(OidIsValid(relid) && SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
      CacheInvalidateRelcacheByRelid(relid);
  }

  return PointerGetDatum(NULL);
}
