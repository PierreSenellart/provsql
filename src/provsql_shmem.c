#include "math.h"

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "access/htup_details.h"
#include "parser/parse_func.h"
#include "storage/shmem.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "utils/uuid.h"

#include "provsql_shmem.h"

shmem_startup_hook_type prev_shmem_startup = NULL;
int provsql_init_nb_gates;
int provsql_max_nb_gates;
int provsql_avg_nb_wires;

static void provsql_shmem_shutdown(int code, Datum arg);

provsqlSharedState *provsql_shared_state = NULL;
HTAB *provsql_hash = NULL;

void provsql_shmem_startup(void)
{
  bool found;
  HASHCTL info;

  if(prev_shmem_startup)
    prev_shmem_startup();

  // Reset in case of restart
  provsql_shared_state = NULL;
  provsql_hash = NULL;

  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  provsql_shared_state = ShmemInitStruct(
      "provsql",
      add_size(offsetof(provsqlSharedState, wires),
        mul_size(sizeof(pg_uuid_t), provsql_max_nb_gates * provsql_avg_nb_wires)),
      &found);

  if(!found) {
#if PG_VERSION_NUM >= 90600
    /* Named lock tranches were added in version 9.6 of PostgreSQL */
    provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
#else
    provsql_shared_state->lock =LWLockAssign();
#endif /* PG_VERSION_NUM >= 90600 */
    provsql_shared_state->nb_wires=0;
  }

  memset(&info, 0, sizeof(info));
  info.keysize = sizeof(pg_uuid_t);
  info.entrysize = sizeof(provsqlHashEntry);

  provsql_hash = ShmemInitHash(
      "provsql hash",
      provsql_init_nb_gates,
      provsql_max_nb_gates,
      &info,
      HASH_ELEM | HASH_BLOBS
  );

  LWLockRelease(AddinShmemInitLock);

  // If we are in the main process, we set up a shutdown hook
  if(!IsUnderPostmaster)
    on_shmem_exit(provsql_shmem_shutdown, (Datum) 0);

  // Already initialized
  if(found)
    return;

  // TODO: Read circuit from file
}

static void provsql_shmem_shutdown(int code, Datum arg)
{
  // TODO: Write circuit to file
}

Size provsql_memsize(void)
{
  Size size = 0;

  // Size of the shared state structure
  size = add_size(size, offsetof(provsqlSharedState, wires));
  // Size of the array of wire ends
  size = add_size(size, mul_size(sizeof(pg_uuid_t), provsql_max_nb_gates * provsql_avg_nb_wires));
  // Size of the hashtable of gates
  size = add_size(size, 
                  hash_estimate_size(provsql_max_nb_gates, sizeof(provsqlHashEntry)));

  return size;
}

PG_FUNCTION_INFO_V1(create_gate);
Datum create_gate(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type = (gate_type) PG_GETARG_INT32(1);
  ArrayType *children = PG_ARGISNULL(2)?NULL:PG_GETARG_ARRAYTYPE_P(2);
  int nb_children = 0;
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to create_gate");

  if(children) {
   if(ARR_NDIM(children) > 1)
     elog(ERROR, "Invalid multi-dimensional array passed to create_gate");
   else if(ARR_NDIM(children) == 1)
     nb_children = *ARR_DIMS(children);
  }

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(hash_get_num_entries(provsql_hash) == provsql_max_nb_gates) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Too many gates in in-memory circuit");
  }

  if(nb_children && provsql_shared_state->nb_wires + nb_children > provsql_max_nb_gates * provsql_avg_nb_wires) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Too many wires in in-memory circuit");
  }

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    entry->type = -1;
    for(int i=0; i<nb_gate_types; ++i) {
      if(provsql_shared_state->constants.GATE_TYPE_TO_OID[i]==type) {
        entry->type = i;
        break;
      }
    }
    if(entry->type == -1)
      elog(ERROR, "Invalid gate type");

    entry->nb_children = nb_children;
    entry->children_idx = provsql_shared_state->nb_wires;
    
    if(nb_children) {
      pg_uuid_t *data = (pg_uuid_t*) ARR_DATA_PTR(children);

      for(int i=0; i<nb_children; ++i) {
        provsql_shared_state->wires[entry->children_idx + i] = data[i];
      }

      provsql_shared_state->nb_wires += nb_children;
    }

    if(entry->type == gate_zero)
      entry->prob = 0.;
    else if(entry->type == gate_one)
      entry->prob = 1.;
    else
      entry->prob = NAN;

    entry->info1 = entry->info2 = 0;
  }

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob);
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_prob");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    hash_search(provsql_hash, token, HASH_REMOVE, &found);
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Unknown gate");
  }
  
  if(entry->type != gate_input && entry->type != gate_mulinput) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Probability can only be assigned to input token");
  }

  entry->prob = prob;

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_infos);
Datum set_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 = PG_GETARG_INT32(1);
  unsigned info2 = PG_GETARG_INT32(2);
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_infos");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    hash_search(provsql_hash, token, HASH_REMOVE, &found);
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Unknown gate");
  }

  if(entry->type == gate_eq && PG_ARGISNULL(2)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Invalid NULL value passed to set_infos");
  }

  if(entry->type != gate_eq && entry->type != gate_mulinput) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Infos cannot be assigned to this gate type");
  }

  entry->info1 = info1;
  if(entry->type == gate_eq)
    entry->info2 = info2;

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_gate_type);
Datum get_gate_type(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  gate_type result = -1;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found)
    result = entry->type;
  
  LWLockRelease(provsql_shared_state->lock);

  if(!found)
    PG_RETURN_NULL();
  else
    PG_RETURN_INT32(provsql_shared_state->constants.GATE_TYPE_TO_OID[result]);
}

PG_FUNCTION_INFO_V1(get_children);
Datum get_children(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  ArrayType *result = NULL;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found) {
    Datum *children_ptr = palloc(entry->nb_children * sizeof(Datum));
    for(int i=0;i<entry->nb_children;++i) {
      children_ptr[i] = UUIDPGetDatum(&provsql_shared_state->wires[entry->children_idx + i]);
    }
    result = construct_array(
        children_ptr,
        entry->nb_children,
        provsql_shared_state->constants.OID_TYPE_PROVENANCE_TOKEN,
        16,
        false,
        'c');
    pfree(children_ptr);
  }
  
  LWLockRelease(provsql_shared_state->lock);

  if(!found)
    PG_RETURN_NULL();
  else
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(get_prob);
Datum get_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  double result = NAN;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found)
    result = entry->prob;
  
  LWLockRelease(provsql_shared_state->lock);

  if(isnan(result))
    PG_RETURN_NULL();
  else
    PG_RETURN_FLOAT8(result);
}

PG_FUNCTION_INFO_V1(get_infos);
Datum get_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  unsigned info1 =0, info2 = 0;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found) {
    info1 = entry->info1;
    info2 = entry->info2;
  }
  
  LWLockRelease(provsql_shared_state->lock);

  if(info1 == 0)
    PG_RETURN_NULL();
  else {
    TupleDesc tupdesc;
    Datum values[2];
    bool nulls[2];

    get_call_result_type(fcinfo,NULL,&tupdesc);
    tupdesc = BlessTupleDesc(tupdesc);

    nulls[0] = false;
    values[0] = Int32GetDatum(info1);
    if(entry->type == gate_eq) {
      nulls[1] = false;
      values[1] = Int32GetDatum(info2);
    } else
      nulls[1] = (entry->type != gate_eq);

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  }
}

static Oid get_func_oid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
      list_make1(makeString(s)),-1,NIL,false,false,false);
  if(fcl)
    return fcl->oid;    
  else
    return 0;
}

static Oid get_provsql_func_oid(char *s)
{
  FuncCandidateList fcl=FuncnameGetCandidates(
      list_make2(makeString("provsql"),makeString(s)),-1,NIL,false,false,false);
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
    *operatorObjectId = oprform->oid;
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
  HeapTuple   tup;
  Oid         ret;
  
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

PG_FUNCTION_INFO_V1(initialize_constants);
Datum initialize_constants(PG_FUNCTION_ARGS)
{
  #define CheckOid(o) if(provsql_shared_state->constants.o==InvalidOid) \
    elog(ERROR, "Could not initialize provsql constants");

  provsql_shared_state->constants.OID_SCHEMA_PROVSQL = get_namespace_oid("provsql", true);
  CheckOid(OID_SCHEMA_PROVSQL);

  provsql_shared_state->constants.OID_TYPE_PROVENANCE_TOKEN = GetSysCacheOid2(
      TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
      Anum_pg_type_oid,
#endif
      CStringGetDatum("provenance_token"),
      ObjectIdGetDatum(provsql_shared_state->constants.OID_SCHEMA_PROVSQL)
  );
  CheckOid(OID_TYPE_PROVENANCE_TOKEN);
  
  provsql_shared_state->constants.OID_TYPE_GATE_TYPE = GetSysCacheOid2(
      TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
      Anum_pg_type_oid,
#endif
      CStringGetDatum("provenance_gate"),
      ObjectIdGetDatum(provsql_shared_state->constants.OID_SCHEMA_PROVSQL)
  );
  CheckOid(OID_TYPE_GATE_TYPE);
  
  provsql_shared_state->constants.OID_TYPE_AGG_TOKEN = GetSysCacheOid2(
      TYPENAMENSP,
#if PG_VERSION_NUM >= 120000
      Anum_pg_type_oid,
#endif
      CStringGetDatum("agg_token"),
      ObjectIdGetDatum(provsql_shared_state->constants.OID_SCHEMA_PROVSQL)
  );
  CheckOid(OID_TYPE_AGG_TOKEN);

  provsql_shared_state->constants.OID_TYPE_UUID = TypenameGetTypid("uuid");
  CheckOid(OID_TYPE_UUID);

  provsql_shared_state->constants.OID_TYPE_UUID_ARRAY = TypenameGetTypid("_uuid");
  CheckOid(OID_TYPE_UUID_ARRAY);
  
  provsql_shared_state->constants.OID_TYPE_INT = TypenameGetTypid("int4");
  CheckOid(OID_TYPE_INT);

  provsql_shared_state->constants.OID_TYPE_INT_ARRAY = TypenameGetTypid("_int4");
  CheckOid(OID_TYPE_INT_ARRAY);
  
  provsql_shared_state->constants.OID_FUNCTION_ARRAY_AGG = get_func_oid("array_agg");
  CheckOid(OID_FUNCTION_ARRAY_AGG);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_PLUS = get_provsql_func_oid("provenance_plus");
  CheckOid(OID_FUNCTION_PROVENANCE_PLUS);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_TIMES = get_provsql_func_oid("provenance_times");
  CheckOid(OID_FUNCTION_PROVENANCE_TIMES);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_MONUS = get_provsql_func_oid("provenance_monus");
  CheckOid(OID_FUNCTION_PROVENANCE_MONUS);
  
  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_PROJECT = get_provsql_func_oid("provenance_project");
  CheckOid(OID_FUNCTION_PROVENANCE_PROJECT);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_EQ = get_provsql_func_oid("provenance_eq");
  CheckOid(OID_FUNCTION_PROVENANCE_EQ);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE = get_provsql_func_oid("provenance");
  CheckOid(OID_FUNCTION_PROVENANCE);
  
  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_DELTA = get_provsql_func_oid("provenance_delta");
  CheckOid(OID_FUNCTION_PROVENANCE_DELTA);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_AGGREGATE = get_provsql_func_oid("provenance_aggregate");
  CheckOid(OID_FUNCTION_PROVENANCE_AGGREGATE);

  provsql_shared_state->constants.OID_FUNCTION_PROVENANCE_SEMIMOD = get_provsql_func_oid("provenance_semimod");
  CheckOid(OID_FUNCTION_PROVENANCE_SEMIMOD);

  provsql_shared_state->constants.OID_FUNCTION_GATE_ZERO = get_provsql_func_oid("gate_zero");
  CheckOid(OID_FUNCTION_GATE_ZERO);

  OperatorGet("<>", PG_CATALOG_NAMESPACE, provsql_shared_state->constants.OID_TYPE_UUID, provsql_shared_state->constants.OID_TYPE_UUID, &provsql_shared_state->constants.OID_OPERATOR_NOT_EQUAL_UUID, &provsql_shared_state->constants.OID_FUNCTION_NOT_EQUAL_UUID);
  CheckOid(OID_OPERATOR_NOT_EQUAL_UUID);
  CheckOid(OID_FUNCTION_NOT_EQUAL_UUID);

  #define GET_GATE_TYPE_OID(x) { \
  provsql_shared_state->constants.GATE_TYPE_TO_OID[gate_ ## x] = get_enum_oid( \
      provsql_shared_state->constants.OID_TYPE_GATE_TYPE, \
      #x);\
  if(provsql_shared_state->constants.GATE_TYPE_TO_OID[gate_ ## x]==InvalidOid) \
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

  PG_RETURN_VOID();
}
