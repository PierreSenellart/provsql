/**
 * @file provsql_utils.c
 * @brief OID lookup, constants cache, and utility functions for ProvSQL.
 *
 * Implements the functions declared in @c provsql_utils.h:
 * - @c get_constants(): retrieves and caches per-database OIDs for all
 *   ProvSQL types, functions, and operators.
 * - @c find_equality_operator(): looks up the @c = operator OID for a
 *   given pair of types.
 *
 * The constants cache is a sorted, dynamically-grown array of
 * @c database_constants_t records (one per PostgreSQL database OID)
 * stored in process-local memory and searched with binary search.
 * The @c reset_constants_cache() SQL function forces a cache invalidation
 * for the current database, which is needed after @c ALTER EXTENSION.
 *
 * Several helper functions (@c get_func_oid, @c get_provsql_func_oid,
 * @c OperatorGet, @c get_enum_oid, @c binary_oper_exact) are adapted
 * from PostgreSQL source code that is not exported as a public API.
 */
#include "postgres.h"
#include "access/htup_details.h"
#include "miscadmin.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"

#include <string.h>

#include "provsql_utils.h"

const char *gate_type_name[] = {
  "input",
  "plus",
  "times",
  "monus",
  "project",
  "zero",
  "one",
  "eq",
  "agg",
  "semimod",
  "cmp",
  "delta",
  "value",
  "mulinput",
  "update",
  "rv",
  "arith",
  "mixture",
  "invalid"
};

/**
 * @brief Look up an exactly matching binary operator OID.
 *
 * Copied and adapted from @c parse_oper.c (PostgreSQL internals, not
 * exported).  Returns @c InvalidOid if no exact match exists.
 *
 * @param opname  Qualified operator name (a @c List of @c String nodes).
 * @param arg1    OID of the left operand type.
 * @param arg2    OID of the right operand type.
 * @return        OID of the matching operator, or @c InvalidOid.
 */
static Oid
binary_oper_exact(List *opname, Oid arg1, Oid arg2)
{
  Oid result;
  bool was_unknown = false;

  /* Unspecified type for one of the arguments? then use the other */
  if ((arg1 == UNKNOWNOID) && (arg2 != InvalidOid))
  {
    arg1 = arg2;
    was_unknown = true;
  }
  else if ((arg2 == UNKNOWNOID) && (arg1 != InvalidOid))
  {
    arg2 = arg1;
    was_unknown = true;
  }

  result = OpernameGetOprid(opname, arg1, arg2);
  if (OidIsValid(result))
    return result;

  if (was_unknown)
  {
    /* arg1 and arg2 are the same here, need only look at arg1 */
    Oid basetype = getBaseType(arg1);

    if (basetype != arg1)
    {
      result = OpernameGetOprid(opname, basetype, basetype);
      if (OidIsValid(result))
        return result;
    }
  }

  return InvalidOid;
}

/* Adapted from PostgreSQL code that is not exported (see parse_oper.c
 * and the static function oper_select_candidate therein).
 */
Oid find_equality_operator(Oid ltypeId, Oid rtypeId)
{
  List * const equals=list_make1(makeString("="));

  FuncCandidateList clist;
  Oid inputOids[2] = {ltypeId,rtypeId};
  int ncandidates;

  Oid result = binary_oper_exact(equals, ltypeId, rtypeId);

  if(result!=InvalidOid)
    return result;

  clist = OpernameGetCandidates(equals, 'b', false);

  ncandidates = func_match_argtypes(2, inputOids,
                                    clist, &clist);

  if (ncandidates == 0)
    return InvalidOid;
  else if (ncandidates == 1)
    return clist->oid;

  clist = func_select_candidate(2, inputOids, clist);

  if(clist)
    return clist->oid;
  else
    return InvalidOid;
}

/**
 * @brief Return the OID of a globally qualified function named @p s.
 *
 * Looks up the function in the default search path.  Returns 0 if no
 * matching function is found.
 *
 * @param s  Function name (unqualified).
 * @return   OID of the function, or 0 if not found.
 */
static Oid get_func_oid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
    list_make1(makeString(s)),
    -1,
    NIL,
    false,
    false,
#if PG_VERSION_NUM >= 140000
    false,
#endif
    false);
  if(fcl)
    return fcl->oid;
  else
    return 0;
}

/**
 * @brief Return the OID of a @c provsql-schema function named @p s.
 *
 * Looks up the function in the @c provsql schema.  Returns 0 if not found.
 *
 * @param s  Function name (without schema prefix).
 * @return   OID of the function, or 0 if not found.
 */
static Oid get_provsql_func_oid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
    list_make2(makeString("provsql"),makeString(s)),
    -1,
    NIL,
    false,
    false,
#if PG_VERSION_NUM >= 140000
    false,
#endif
    false);
  if(fcl)
    return fcl->oid;
  else
    return 0;
}

/**
 * @brief Retrieve operator and function OIDs for a named operator.
 *
 * Copied and adapted from @c pg_operator.c (PostgreSQL internals, not
 * exported).  Looks up the operator by name, namespace, and operand types
 * in the system cache.
 *
 * @param operatorName      Operator symbol string (e.g. @c "<>").
 * @param operatorNamespace OID of the schema containing the operator.
 * @param leftObjectId      OID of the left operand type.
 * @param rightObjectId     OID of the right operand type.
 * @param operatorObjectId  Output: OID of the operator, or 0 if not found.
 * @param functionObjectId  Output: OID of the underlying function, or 0.
 */
static void OperatorGet(
  const char *operatorName,
  Oid operatorNamespace,
  Oid leftObjectId,
  Oid rightObjectId,
  Oid *operatorObjectId,
  Oid *functionObjectId)
{
  HeapTuple tup;
  bool defined;

  tup = SearchSysCache4(OPERNAMENSP,
                        PointerGetDatum(operatorName),
                        ObjectIdGetDatum(leftObjectId),
                        ObjectIdGetDatum(rightObjectId),
                        ObjectIdGetDatum(operatorNamespace));
  if (HeapTupleIsValid(tup))
  {
    Form_pg_operator oprform = (Form_pg_operator) GETSTRUCT(tup);
#if PG_VERSION_NUM >= 120000
    *operatorObjectId = oprform->oid;
#else
    *operatorObjectId = HeapTupleGetOid(tup);
#endif
    *functionObjectId = oprform->oprcode;
    defined = RegProcedureIsValid(oprform->oprcode);
    ReleaseSysCache(tup);
  }
  else
  {
    defined = false;
  }

  if(!defined) {
    *operatorObjectId = 0;
    *functionObjectId = 0;
  }
}

/**
 * @brief Return the OID of a specific enum label within an enum type.
 *
 * @param enumtypoid  OID of the enum type (e.g. @c provenance_gate).
 * @param label       C-string label of the enum value to look up.
 * @return            OID of the enum label's @c pg_enum row, or
 *                    @c InvalidOid if the label is not present.
 */
static Oid get_enum_oid(Oid enumtypoid, const char *label)
{
  HeapTuple tup;
  Oid ret;

  tup = SearchSysCache2(ENUMTYPOIDNAME,
                        ObjectIdGetDatum(enumtypoid),
                        CStringGetDatum(label));
  if (!HeapTupleIsValid(tup))
    return InvalidOid;

#if PG_VERSION_NUM >= 120000
  ret = ((Form_pg_enum) GETSTRUCT(tup))->oid;
#else
  ret = HeapTupleGetOid(tup);
#endif

  ReleaseSysCache(tup);

  return ret;
}

/**
 * @brief Query the system catalogs to populate a fresh @c constants_t.
 *
 * Performs all OID lookups required by ProvSQL in a single pass through
 * the system caches.  The @c CheckOid() macro aborts (or returns early,
 * depending on @p failure_if_not_possible) if any OID resolves to
 * @c InvalidOid.
 *
 * @param failure_if_not_possible  If @c true, raise a @c provsql_error when
 *        any OID cannot be resolved.  If @c false, return a @c constants_t
 *        with @c ok==false instead.
 * @return  Fully populated @c constants_t on success, or @c ok==false on
 *          failure when @p failure_if_not_possible is @c false.
 */
static constants_t initialize_constants(bool failure_if_not_possible)
{
  constants_t constants;
  constants.ok = false;

  /** @brief Abort or return early if OID field @p o of @p constants is invalid. */
  #define CheckOid(o) if(constants.o==InvalidOid) { \
            if(failure_if_not_possible) \
            provsql_error("Could not initialize provsql constants"); \
            else \
            return constants; }

  constants.OID_SCHEMA_PROVSQL = get_namespace_oid("provsql", true);
  CheckOid(OID_SCHEMA_PROVSQL);

  constants.OID_TYPE_UUID = TypenameGetTypid("uuid");
  CheckOid(OID_TYPE_UUID);

  constants.OID_TYPE_GATE_TYPE = GetSysCacheOid2(
    TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
    Anum_pg_type_oid,
#endif
    CStringGetDatum("provenance_gate"),
    ObjectIdGetDatum(constants.OID_SCHEMA_PROVSQL)
    );
  CheckOid(OID_TYPE_GATE_TYPE);

  constants.OID_TYPE_AGG_TOKEN = GetSysCacheOid2(
    TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
    Anum_pg_type_oid,
#endif
    CStringGetDatum("agg_token"),
    ObjectIdGetDatum(constants.OID_SCHEMA_PROVSQL)
    );
  CheckOid(OID_TYPE_AGG_TOKEN);

  constants.OID_TYPE_UUID = TypenameGetTypid("uuid");
  CheckOid(OID_TYPE_UUID);

  constants.OID_TYPE_UUID_ARRAY = TypenameGetTypid("_uuid");
  CheckOid(OID_TYPE_UUID_ARRAY);

  constants.OID_TYPE_INT = TypenameGetTypid("int4");
  CheckOid(OID_TYPE_INT);

  constants.OID_TYPE_BOOL = TypenameGetTypid("bool");
  CheckOid(OID_TYPE_BOOL);

  constants.OID_TYPE_FLOAT = TypenameGetTypid("float8");
  CheckOid(OID_TYPE_FLOAT);

  constants.OID_TYPE_INT_ARRAY = TypenameGetTypid("_int4");
  CheckOid(OID_TYPE_INT_ARRAY);

  constants.OID_TYPE_VARCHAR = TypenameGetTypid("varchar");
  CheckOid(OID_TYPE_VARCHAR);

#if PG_VERSION_NUM >= 140000
  constants.OID_TYPE_TSTZMULTIRANGE = TypenameGetTypid("tstzmultirange");
  CheckOid(OID_TYPE_TSTZMULTIRANGE);
  constants.OID_TYPE_NUMMULTIRANGE = TypenameGetTypid("nummultirange");
  CheckOid(OID_TYPE_NUMMULTIRANGE);
  constants.OID_TYPE_INT4MULTIRANGE = TypenameGetTypid("int4multirange");
  CheckOid(OID_TYPE_INT4MULTIRANGE);
#else
  constants.OID_TYPE_TSTZMULTIRANGE = InvalidOid;
  constants.OID_TYPE_NUMMULTIRANGE = InvalidOid;
  constants.OID_TYPE_INT4MULTIRANGE = InvalidOid;
#endif

  constants.OID_FUNCTION_ARRAY_AGG = get_func_oid("array_agg");
  CheckOid(OID_FUNCTION_ARRAY_AGG);

  constants.OID_FUNCTION_PROVENANCE_PLUS = get_provsql_func_oid("provenance_plus");
  CheckOid(OID_FUNCTION_PROVENANCE_PLUS);

  constants.OID_FUNCTION_PROVENANCE_TIMES = get_provsql_func_oid("provenance_times");
  CheckOid(OID_FUNCTION_PROVENANCE_TIMES);

  constants.OID_FUNCTION_PROVENANCE_MONUS = get_provsql_func_oid("provenance_monus");
  CheckOid(OID_FUNCTION_PROVENANCE_MONUS);

  constants.OID_FUNCTION_PROVENANCE_PROJECT = get_provsql_func_oid("provenance_project");
  CheckOid(OID_FUNCTION_PROVENANCE_PROJECT);

  constants.OID_FUNCTION_PROVENANCE_EQ = get_provsql_func_oid("provenance_eq");
  CheckOid(OID_FUNCTION_PROVENANCE_EQ);

  constants.OID_FUNCTION_PROVENANCE = get_provsql_func_oid("provenance");
  CheckOid(OID_FUNCTION_PROVENANCE);

  constants.OID_FUNCTION_PROVENANCE_DELTA = get_provsql_func_oid("provenance_delta");
  CheckOid(OID_FUNCTION_PROVENANCE_DELTA);

  constants.OID_FUNCTION_PROVENANCE_AGGREGATE = get_provsql_func_oid("provenance_aggregate");
  CheckOid(OID_FUNCTION_PROVENANCE_AGGREGATE);

  constants.OID_FUNCTION_PROVENANCE_SEMIMOD = get_provsql_func_oid("provenance_semimod");
  CheckOid(OID_FUNCTION_PROVENANCE_SEMIMOD);

  constants.OID_FUNCTION_GATE_ZERO = get_provsql_func_oid("gate_zero");
  CheckOid(OID_FUNCTION_GATE_ZERO);

  constants.OID_FUNCTION_GATE_ONE = get_provsql_func_oid("gate_one");
  CheckOid(OID_FUNCTION_GATE_ONE);

  constants.OID_FUNCTION_PROVENANCE_CMP = get_provsql_func_oid("provenance_cmp");
  CheckOid(OID_FUNCTION_PROVENANCE_CMP);

  constants.OID_FUNCTION_AGG_TOKEN_UUID = get_provsql_func_oid("agg_token_uuid");
  CheckOid(OID_FUNCTION_AGG_TOKEN_UUID);

  /* random_variable type and its operator procedures will ship in
   * 1.5.0.  Older schemas (notably the 1.0.0 baseline used by
   * extension_upgrade) do not have them.  Treat each lookup as
   * optional -- if the catalog lacks the symbol, the OID stays
   * InvalidOid and downstream code (rv_cmp_index, the planner-hook
   * walker) silently no-ops on such schemas because real OpExpr
   * funcoids never equal InvalidOid.  Mirrors the
   * GET_GATE_TYPE_OID_OPTIONAL pattern above. */
  constants.OID_TYPE_RANDOM_VARIABLE = GetSysCacheOid2(
    TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
    Anum_pg_type_oid,
#endif
    CStringGetDatum("random_variable"),
    ObjectIdGetDatum(constants.OID_SCHEMA_PROVSQL)
    );

  /* rv_aggregate_semimod helper used by the RV-returning aggregate
   * rewrite (sum, avg, and any future aggregate whose result type is
   * random_variable).  The planner-hook routes on aggtype instead of a
   * per-aggregate OID, so no individual aggregate OID needs to be
   * cached here; an InvalidOid for this helper just leaves the rewrite
   * disabled on older schemas that lack the continuous-distribution
   * surface (1.0.0 baseline used by extension_upgrade). */
  constants.OID_FUNCTION_RV_AGGREGATE_SEMIMOD =
    get_provsql_func_oid("rv_aggregate_semimod");

  /* random_variable_{eq,ne,le,lt,ge,gt} -- order matches the
   * ComparisonOperator enum in src/Aggregation.h (EQ=0, NE=1, LE=2,
   * LT=3, GE=4, GT=5). */
  constants.OID_FUNCTION_RV_CMP[0] = get_provsql_func_oid("random_variable_eq");
  constants.OID_FUNCTION_RV_CMP[1] = get_provsql_func_oid("random_variable_ne");
  constants.OID_FUNCTION_RV_CMP[2] = get_provsql_func_oid("random_variable_le");
  constants.OID_FUNCTION_RV_CMP[3] = get_provsql_func_oid("random_variable_lt");
  constants.OID_FUNCTION_RV_CMP[4] = get_provsql_func_oid("random_variable_ge");
  constants.OID_FUNCTION_RV_CMP[5] = get_provsql_func_oid("random_variable_gt");

  OperatorGet("<>", PG_CATALOG_NAMESPACE, constants.OID_TYPE_UUID, constants.OID_TYPE_UUID, &constants.OID_OPERATOR_NOT_EQUAL_UUID, &constants.OID_FUNCTION_NOT_EQUAL_UUID);
  CheckOid(OID_OPERATOR_NOT_EQUAL_UUID);
  CheckOid(OID_FUNCTION_NOT_EQUAL_UUID);

  /** @brief Look up the OID of provenance_gate enum value @p x and store it in constants. */
  #define GET_GATE_TYPE_OID(x) { \
            constants.GATE_TYPE_TO_OID[gate_ ## x] = get_enum_oid( \
              constants.OID_TYPE_GATE_TYPE, \
              #x); \
            if(constants.GATE_TYPE_TO_OID[gate_ ## x]==InvalidOid) \
            provsql_error("Could not initialize provsql gate type " #x); }

  /** @brief Like @c GET_GATE_TYPE_OID but tolerates a missing enum value.
   *
   * Used for gate types added in releases newer than the oldest schema
   * the @c extension_upgrade test exercises (currently 1.0.0).  An
   * intermediate state where a 1.0.0 database has been bound to a newer
   * shared library is possible (e.g. between @c CREATE @c EXTENSION
   * @c VERSION and @c ALTER @c EXTENSION @c UPDATE) and must not abort
   * @c get_constants -- the missing OID stays @c InvalidOid and any
   * attempt to actually create such a gate fails later in
   * @c create_gate's "Invalid gate type" branch.  When the upgrade
   * scripts catch up, the lookup succeeds and the gate becomes usable
   * normally.  No state for @c InvalidOid is stored, so the field
   * keeps its zero-init value (which is @c InvalidOid).
   */
  #define GET_GATE_TYPE_OID_OPTIONAL(x) { \
            constants.GATE_TYPE_TO_OID[gate_ ## x] = get_enum_oid( \
              constants.OID_TYPE_GATE_TYPE, \
              #x); }

  GET_GATE_TYPE_OID(input);
  GET_GATE_TYPE_OID(plus);
  GET_GATE_TYPE_OID(times);
  GET_GATE_TYPE_OID(monus);
  GET_GATE_TYPE_OID(project);
  GET_GATE_TYPE_OID(zero);
  GET_GATE_TYPE_OID(one);
  GET_GATE_TYPE_OID(eq);
  GET_GATE_TYPE_OID(agg);
  GET_GATE_TYPE_OID(semimod);
  GET_GATE_TYPE_OID(cmp);
  GET_GATE_TYPE_OID(delta);
  GET_GATE_TYPE_OID(value);
  GET_GATE_TYPE_OID(mulinput);
  GET_GATE_TYPE_OID(update);
  GET_GATE_TYPE_OID_OPTIONAL(rv);
  GET_GATE_TYPE_OID_OPTIONAL(arith);
  GET_GATE_TYPE_OID_OPTIONAL(mixture);

  constants.ok=true;

  return constants;
}

static database_constants_t *constants_cache; ///< Per-database OID constants cache (sorted by database OID)
static unsigned constants_cache_len=0;        ///< Number of valid entries in @c constants_cache

constants_t get_constants(bool failure_if_not_possible)
{
  int start=0, end=constants_cache_len-1;
  database_constants_t *constants_cache2;


  while(end>=start) {
    unsigned mid=(start+end)/2;
    if(constants_cache[mid].database<MyDatabaseId)
      start=mid+1;
    else if(constants_cache[mid].database>MyDatabaseId)
      end=mid-1;
    else
      return constants_cache[mid].constants;
  }

  constants_cache2=calloc(constants_cache_len+1, sizeof(database_constants_t));
  for(unsigned i=0; i<start; ++i)
    constants_cache2[i]=constants_cache[i];

  constants_cache2[start].database=MyDatabaseId;
  constants_cache2[start].constants=initialize_constants(failure_if_not_possible);

  for(unsigned i=start; i<constants_cache_len; ++i)
    constants_cache2[i+1]=constants_cache[i];
  free(constants_cache);
  constants_cache=constants_cache2;
  ++constants_cache_len;

  return constants_cache[start].constants;
}

/* -------------------------------------------------------------------------
 * Per-backend table-info cache
 *
 * Sorted array of @c table_info_cache_entry, binary-searched on @c relid.
 * Used by @c provsql_lookup_table_info to amortise IPC across repeated
 * lookups during query planning.  Entries are invalidated by
 * @c invalidate_table_info_cache_callback, which is hooked into
 * PostgreSQL's relcache invalidation channel and so reacts to local
 * DDL, cross-backend invalidations broadcast via
 * @c CacheInvalidateRelcacheByRelid, and explicit "invalidate all"
 * (relid == InvalidOid) events.
 * ------------------------------------------------------------------------- */

typedef struct table_info_cache_entry {
  Oid           relid;                                          ///< pg_class OID (sort key)
  bool          valid;                                          ///< false => refresh on next access
  bool          present;                                        ///< when valid: was a record found at the worker?
  uint8         kind;                                           ///< when present: provsql_table_kind value
  uint16        block_key_n;                                    ///< when present: number of block-key columns
  AttrNumber    block_key[PROVSQL_TABLE_INFO_MAX_BLOCK_KEY];    ///< when present: block-key column numbers
} table_info_cache_entry;

static table_info_cache_entry *table_info_cache = NULL; ///< Sorted by @c relid
static unsigned table_info_cache_len = 0;
static bool table_info_callback_registered = false;

/** Find @p relid in the cache.  Returns the index on hit; otherwise
 *  @c -1 and writes the insertion point to @p *insert_at. */
static int table_info_cache_find(Oid relid, int *insert_at)
{
  int start = 0, end = (int)table_info_cache_len - 1;
  while(end >= start) {
    int mid = (start + end) / 2;
    if(table_info_cache[mid].relid < relid)
      start = mid + 1;
    else if(table_info_cache[mid].relid > relid)
      end = mid - 1;
    else
      return mid;
  }
  if(insert_at) *insert_at = start;
  return -1;
}

/** Insert a fresh entry at @p pos (which must be the value returned by
 *  the most recent @c table_info_cache_find that reported a miss). */
static void table_info_cache_insert(int pos, Oid relid, bool present,
                                    const ProvenanceTableInfo *info)
{
  table_info_cache_entry *new_buf = calloc(table_info_cache_len + 1,
                                           sizeof(table_info_cache_entry));
  for(int i = 0; i < pos; ++i)
    new_buf[i] = table_info_cache[i];
  new_buf[pos].relid       = relid;
  new_buf[pos].valid       = true;
  new_buf[pos].present     = present;
  if(present) {
    new_buf[pos].kind        = info->kind;
    new_buf[pos].block_key_n = info->block_key_n;
    memcpy(new_buf[pos].block_key, info->block_key,
           info->block_key_n * sizeof(AttrNumber));
  }
  for(unsigned i = (unsigned)pos; i < table_info_cache_len; ++i)
    new_buf[i + 1] = table_info_cache[i];
  free(table_info_cache);
  table_info_cache = new_buf;
  ++table_info_cache_len;
}

/** Relcache callback: PostgreSQL fires this whenever a relation's
 *  relcache entry is invalidated (locally or via shared invalidation
 *  from another backend).  @p relid == @c InvalidOid means
 *  "invalidate everything." */
static void invalidate_table_info_cache_callback(Datum arg, Oid relid)
{
  int pos;
  (void) arg;
  if(relid == InvalidOid) {
    for(unsigned i = 0; i < table_info_cache_len; ++i)
      table_info_cache[i].valid = false;
    return;
  }
  pos = table_info_cache_find(relid, NULL);
  if(pos >= 0)
    table_info_cache[pos].valid = false;
}

bool provsql_lookup_table_info(Oid relid, ProvenanceTableInfo *out)
{
  int insert_at = 0;
  int pos;
  ProvenanceTableInfo info;
  bool present;

  if(!table_info_callback_registered) {
    CacheRegisterRelcacheCallback(invalidate_table_info_cache_callback,
                                  (Datum) 0);
    table_info_callback_registered = true;
  }

  pos = table_info_cache_find(relid, &insert_at);

  if(pos >= 0 && table_info_cache[pos].valid) {
    table_info_cache_entry *e = &table_info_cache[pos];
    if(!e->present)
      return false;
    out->relid       = relid;
    out->kind        = e->kind;
    out->block_key_n = e->block_key_n;
    memcpy(out->block_key, e->block_key,
           e->block_key_n * sizeof(AttrNumber));
    return true;
  }

  present = provsql_fetch_table_info(relid, &info);

  if(pos >= 0) {
    /* Refresh an existing, now-stale slot in place. */
    table_info_cache_entry *e = &table_info_cache[pos];
    e->valid   = true;
    e->present = present;
    if(present) {
      e->kind        = info.kind;
      e->block_key_n = info.block_key_n;
      memcpy(e->block_key, info.block_key,
             info.block_key_n * sizeof(AttrNumber));
    }
  } else {
    table_info_cache_insert(insert_at, relid, present, &info);
  }

  if(present) {
    *out = info;
    return true;
  }
  return false;
}

PG_FUNCTION_INFO_V1(reset_constants_cache);
/**
 * @brief SQL function to invalidate the OID constants cache.
 *
 * Forces a fresh OID lookup for the current database on the next call to
 * @c get_constants().  Must be called after @c ALTER EXTENSION provsql
 * UPDATE to ensure cached OIDs are refreshed.
 * @return Void datum.
 */
Datum reset_constants_cache(PG_FUNCTION_ARGS)
{
  int start=0, end=constants_cache_len-1;

  while(end>=start) {
    unsigned mid=(start+end)/2;
    if(constants_cache[mid].database<MyDatabaseId)
      start=mid+1;
    else if(constants_cache[mid].database>MyDatabaseId)
      end=mid-1;
    else {
      constants_cache[mid].constants = initialize_constants(true);
      break;
    }
  }

  PG_RETURN_VOID();
}
