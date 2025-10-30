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

#include "provsql_utils.h"

/* Copied over from parse_oper.c as defined static there */
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

/** Return the OID of an equality operator. This is adapted from existing
 * PostgreSQL code that is not exported (see in particular parse_oper.c,
 * and the static function oper_select_candidate therein).
 *
 * \param ltypeId the OID of the type of the first operand
 * \param rtypeId the OID of the type of the second operand
 * \return the OID of the equality operator for these two operands
 * */
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

// Copied over from pg_operator.c as defined static there, with
// various modifications
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

static Oid get_enum_oid(Oid enumtypoid, const char *label)
{
  HeapTuple tup;
  Oid ret;

  tup = SearchSysCache2(ENUMTYPOIDNAME,
                        ObjectIdGetDatum(enumtypoid),
                        CStringGetDatum(label));
  Assert(HeapTupleIsValid(tup));

#if PG_VERSION_NUM >= 120000
  ret = ((Form_pg_enum) GETSTRUCT(tup))->oid;
#else
  ret = HeapTupleGetOid(tup);
#endif

  ReleaseSysCache(tup);

  return ret;
}

/** Returns an initialized constants_t structure by querying the database
 * for all OIDs.
 *
 * \param failure_if_not_possible indicates whether a failure should
 * result in an error (at the PostgreSQL level) or should be silently
 * ignored
 * \return the initialized structure
 */
static constants_t initialize_constants(bool failure_if_not_possible)
{
  constants_t constants;
  constants.ok = false;

  #define CheckOid(o) if(constants.o==InvalidOid) { \
            if(failure_if_not_possible) \
            elog(ERROR, "Could not initialize provsql constants"); \
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

  OperatorGet("<>", PG_CATALOG_NAMESPACE, constants.OID_TYPE_UUID, constants.OID_TYPE_UUID, &constants.OID_OPERATOR_NOT_EQUAL_UUID, &constants.OID_FUNCTION_NOT_EQUAL_UUID);
  CheckOid(OID_OPERATOR_NOT_EQUAL_UUID);
  CheckOid(OID_FUNCTION_NOT_EQUAL_UUID);

  #define GET_GATE_TYPE_OID(x) { \
            constants.GATE_TYPE_TO_OID[gate_ ## x] = get_enum_oid( \
              constants.OID_TYPE_GATE_TYPE, \
              #x); \
            if(constants.GATE_TYPE_TO_OID[gate_ ## x]==InvalidOid) \
            elog(ERROR, "Could not initialize provsql gate type " #x); }

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

  constants.ok=true;

  return constants;
}

database_constants_t *constants_cache;
unsigned constants_cache_len=0;

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

PG_FUNCTION_INFO_V1(reset_constants_cache);
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
