#include "math.h"

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "parser/parse_func.h"
#include "storage/shmem.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/hsearch.h"
#include "utils/uuid.h"

#include "unistd.h"

#include "provsql_shmem.h"

#define PROVSQL_DUMP_FILE "provsql.tmp"

shmem_startup_hook_type prev_shmem_startup = NULL;
#if (PG_VERSION_NUM >= 150000)
shmem_request_hook_type prev_shmem_request = NULL;
#endif
int provsql_init_nb_gates;
int provsql_max_nb_gates;
int provsql_avg_nb_wires;

static void provsql_shmem_shutdown(int code, Datum arg);

provsqlSharedState *provsql_shared_state = NULL;
HTAB *provsql_hash = NULL;
provsqlHashEntry *entry;

static Size provsql_struct_size(void)
{
  return add_size(offsetof(provsqlSharedState, wires),
                  mul_size(sizeof(pg_uuid_t), provsql_max_nb_gates * provsql_avg_nb_wires));
}

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
    provsql_struct_size(),
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


  if( access( "provsql.tmp", F_OK ) == 0 ) {
    switch (provsql_deserialize("provsql.tmp"))
    {
    case 1:
      //elog(ERROR, "Error while opening the file during deserialization");
      break;

    case 2:
      //elog(ERROR, "Error while reading the file during deserialization");
      break;

    case 3:
      elog(ERROR, "Error while closing the file during deserialization");
      break;
    }
  }

}

static void provsql_shmem_shutdown(int code, Datum arg)
{

  #if PG_VERSION_NUM >= 90600
  // Named lock tranches were added in version 9.6 of PostgreSQL
  provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
  #else
  provsql_shared_state->lock =LWLockAssign();
  #endif // PG_VERSION_NUM >= 90600

  switch (provsql_serialize("provsql.tmp"))
  {
  case 1:
    elog(INFO, "Error while opening the file during serialization");
    break;

  case 2:
    elog(INFO, "Error while writing to file during serialization");
    break;

  case 3:
    elog(INFO, "Error while closing the file during serialization");
    break;
  }

  LWLockRelease(provsql_shared_state->lock);


  // TODO (void) durable_rename(PROVSQL_DUMP_FILE ".tmp", PROVSQL_DUMP_FILE, LOG);

}

Size provsql_memsize(void)
{
  // Size of the shared state structure
  Size size = MAXALIGN(provsql_struct_size());
  // Size of the array of wire ends
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

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    constants_t constants=initialize_constants(true);

    if(nb_children && provsql_shared_state->nb_wires + nb_children > provsql_max_nb_gates * provsql_avg_nb_wires) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Too many wires in in-memory circuit");
    }

    entry->type = -1;
    for(int i=0; i<nb_gate_types; ++i) {
      if(constants.GATE_TYPE_TO_OID[i]==type) {
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
  else {
    constants_t constants=initialize_constants(true);
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[result]);
  }
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
    constants_t constants=initialize_constants(true);
    for(int i=0; i<entry->nb_children; ++i) {
      children_ptr[i] = UUIDPGetDatum(&provsql_shared_state->wires[entry->children_idx + i]);
    }
    result = construct_array(
      children_ptr,
      entry->nb_children,
      constants.OID_TYPE_UUID,
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
void provsql_shmem_request(void)
{
#if (PG_VERSION_NUM >= 150000)
  if (prev_shmem_request)
    prev_shmem_request();
#endif

  RequestAddinShmemSpace(provsql_memsize());

#if PG_VERSION_NUM >= 90600
  /* Named lock tranches were added in version 9.6 of PostgreSQL */
  RequestNamedLWLockTranche("provsql", 1);
#else
  RequestAddinLWLocks(1);
#endif /* PG_VERSION_NUM >= 90600 */
}
