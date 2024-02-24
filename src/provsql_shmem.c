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
#include "provsql_mmap.h"

#define PROVSQL_DUMP_FILE "provsql.tmp"

shmem_startup_hook_type prev_shmem_startup = NULL;
#if (PG_VERSION_NUM >= 150000)
shmem_request_hook_type prev_shmem_request = NULL;
#endif
int provsql_init_nb_gates;
int provsql_max_nb_gates;
int provsql_avg_nb_wires;

provsqlSharedState *provsql_shared_state = NULL;

void provsql_shmem_startup(void)
{
  bool found;
  int pipes_b_to_m[2];
  int pipes_m_to_b[2];

  if(prev_shmem_startup)
    prev_shmem_startup();

  // Reset in case of restart
  provsql_shared_state = NULL;

  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  provsql_shared_state = ShmemInitStruct(
    "provsql",
    sizeof(provsql_shared_state),
    &found);

  if(!found) {
    provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
  }

  LWLockRelease(AddinShmemInitLock);

  // Already initialized
  if(found)
    return;

  if(pipe(pipes_b_to_m) || pipe(pipes_m_to_b))
    elog(ERROR, "Cannot create pipe to communicate with MMap worker");

  provsql_shared_state->pipebmr=pipes_b_to_m[0];
  provsql_shared_state->pipebmw=pipes_b_to_m[1];
  provsql_shared_state->pipembr=pipes_m_to_b[0];
  provsql_shared_state->pipembw=pipes_m_to_b[1];
}

Size provsql_memsize(void)
{
  return MAXALIGN(sizeof(provsqlSharedState));
}

PG_FUNCTION_INFO_V1(create_gate);
Datum create_gate(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  Oid oid_type = PG_GETARG_INT32(1);
  ArrayType *children = PG_ARGISNULL(2)?NULL:PG_GETARG_ARRAYTYPE_P(2);
  unsigned nb_children = 0;
  gate_type type = gate_invalid;
  constants_t constants;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to create_gate");

  if(children) {
    if(ARR_NDIM(children) > 1)
      elog(ERROR, "Invalid multi-dimensional array passed to create_gate");
    else if(ARR_NDIM(children) == 1)
      nb_children = *ARR_DIMS(children);
  }

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  constants=initialize_constants(true);

  for(int i=0; i<nb_gate_types; ++i) {
    if(constants.GATE_TYPE_TO_OID[i]==oid_type) {
      type = i;
      break;
    }
  }
  if(type == gate_invalid) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Invalid gate type");
  }

  if(!WRITEM("C", char)
     || !WRITEM(token, pg_uuid_t)
     || !WRITEM(&type, gate_type)
     || !WRITEM(&nb_children, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe");
  }

  if(nb_children) {
    pg_uuid_t *data = (pg_uuid_t*) ARR_DATA_PTR(children);

    for(int i=0; i<nb_children; ++i) {
      if(!WRITEM(&data[i], pg_uuid_t)) {
        elog(ERROR, "Cannot write to pipe");
        LWLockRelease(provsql_shared_state->lock);
      }
    }
  }

  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob);
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_prob");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("P", char)
     || !WRITEM(token, pg_uuid_t)
     || !WRITEM(&prob, double)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe");
  }

  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_infos);
Datum set_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 = PG_GETARG_INT32(1);
  unsigned info2 = PG_GETARG_INT32(2);

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_infos");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("I", char)
     || !WRITEM(token, pg_uuid_t)
     || !WRITEM(&info1, int)
     || !WRITEM(&info2, int)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type I)");
  }

  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_nb_gates);
Datum get_nb_gates(PG_FUNCTION_ARGS)
{
  unsigned nb;

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("n", char)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type n)");
  }

  if(!READB(nb, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type n)");
  }

  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_INT64((long) nb);
}

PG_FUNCTION_INFO_V1(get_children);
Datum get_children(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  ArrayType *result = NULL;
  unsigned nb_children;
  pg_uuid_t *children;
  Datum *children_ptr;
  constants_t constants;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("c", char) || !WRITEM(token, pg_uuid_t)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type c)");
  }

  if(!READB(nb_children, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type c)");
  }

  children_ptr = palloc(nb_children * sizeof(Datum));
  constants=initialize_constants(true);
  children=calloc(nb_children, sizeof(pg_uuid_t));
  for(unsigned i=0; i<nb_children; ++i) {
    if(!READB(children[i], pg_uuid_t)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot read response from pipe (message type c)");
    }
    children_ptr[i] = UUIDPGetDatum(&children[i]);
  }

  LWLockRelease(provsql_shared_state->lock);

  result = construct_array(
    children_ptr,
    nb_children,
    constants.OID_TYPE_UUID,
    16,
    false,
    'c');
  pfree(children_ptr);
  free(children);

  PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(get_prob);
Datum get_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double result;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("p", char) || !WRITEM(token, pg_uuid_t)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type p)");
  }

  if(!READB(result, double)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type p)");
  }

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
  unsigned info1 =0, info2 = 0;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("i", char) || !WRITEM(token, pg_uuid_t)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type i)");
  }

  if(!READB(info1, int) || !READB(info2, int)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type i)");
  }

  LWLockRelease(provsql_shared_state->lock);

  if(info1 == 0)
    PG_RETURN_NULL();
  else {
    TupleDesc tupdesc;
    Datum values[2];
    bool nulls[2] = {false, false};

    get_call_result_type(fcinfo,NULL,&tupdesc);
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = Int32GetDatum(info1);
    values[1] = Int32GetDatum(info2);

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

  RequestNamedLWLockTranche("provsql", 1);
}
