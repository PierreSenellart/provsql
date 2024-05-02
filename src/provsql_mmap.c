#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <math.h>
#include <assert.h>

#include "postgres.h"
#include "postmaster/bgworker.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

char buffer[PIPE_BUF]={}; // flawfinder: ignore
unsigned bufferpos=0;

__attribute__((visibility("default")))
void provsql_mmap_worker(Datum ignored)
{
  BackgroundWorkerUnblockSignals();
  initialize_provsql_mmap();
  close(provsql_shared_state->pipebmw);
  close(provsql_shared_state->pipembr);
  elog(LOG, "%s initialized", MyBgworkerEntry->bgw_name);

  provsql_mmap_main_loop();

  destroy_provsql_mmap();
}

void RegisterProvSQLMMapWorker(void)
{
  BackgroundWorker worker;

  snprintf(worker.bgw_name, BGW_MAXLEN, "ProvSQL MMap Worker");
#if PG_VERSION_NUM >= 110000
  snprintf(worker.bgw_type, BGW_MAXLEN, "ProvSQL MMap");
#endif

  worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
  worker.bgw_start_time = BgWorkerStart_PostmasterStart;
  worker.bgw_restart_time = 1;

  snprintf(worker.bgw_library_name, BGW_MAXLEN, "provsql");
  snprintf(worker.bgw_function_name, BGW_MAXLEN, "provsql_mmap_worker");
#if PG_VERSION_NUM < 100000
  worker.bgw_main = NULL;
#endif

  worker.bgw_main_arg = (Datum) 0;
  worker.bgw_notify_pid = 0;

  RegisterBackgroundWorker(&worker);
}

PG_FUNCTION_INFO_V1(get_gate_type);
Datum get_gate_type(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type = gate_invalid;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("t", char);
  ADDWRITEM(token, pg_uuid_t);

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!SENDWRITEM() || !READB(type, gate_type)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate on pipe (message type t)");
  }

  LWLockRelease(provsql_shared_state->lock);

  if(type==gate_invalid)
    PG_RETURN_NULL();
  else {
    constants_t constants=get_constants(true);
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]);
  }
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
  pg_uuid_t *children_data;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to create_gate");

  if(children) {
    if(ARR_NDIM(children) > 1)
      elog(ERROR, "Invalid multi-dimensional array passed to create_gate");
    else if(ARR_NDIM(children) == 1)
      nb_children = *ARR_DIMS(children);
  }

  constants=get_constants(true);

  for(int i=0; i<nb_gate_types; ++i) {
    if(constants.GATE_TYPE_TO_OID[i]==oid_type) {
      type = i;
      break;
    }
  }
  if(type == gate_invalid) {
    elog(ERROR, "Invalid gate type");
  }

  if(nb_children>0)
    children_data = (pg_uuid_t*) ARR_DATA_PTR(children);
  else
    children_data = NULL;

  STARTWRITEM();
  ADDWRITEM("C", char);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&type, gate_type);
  ADDWRITEM(&nb_children, unsigned);

  if(PIPE_BUF-bufferpos>nb_children*sizeof(pg_uuid_t)) {
    // Enough space in the buffer for an atomic write, no need of
    // exclusive locks

    for(int i=0; i<nb_children; ++i)
      ADDWRITEM(&children_data[i], pg_uuid_t);

    LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
    if(!SENDWRITEM()) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type C)");
    }
    LWLockRelease(provsql_shared_state->lock);
  } else {
    // Not enough space in buffer, pipe write won't be atomic, we need to
    // make several writes and use locks
    unsigned children_per_batch = PIPE_BUF/sizeof(pg_uuid_t);

    LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

    if(!SENDWRITEM()) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type C)");
    }

    for(unsigned j=0; j<1+(nb_children-1)/children_per_batch; ++j) {
      STARTWRITEM();

      for(unsigned i=j*children_per_batch; i<(j+1)*children_per_batch && i<nb_children; ++i) {
        ADDWRITEM(&children_data[i], pg_uuid_t);
      }

      if(!SENDWRITEM()) {
        LWLockRelease(provsql_shared_state->lock);
        elog(ERROR, "Cannot write to pipe (message type C)");
      }
    }

    LWLockRelease(provsql_shared_state->lock);
  }

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob);
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_prob");

  STARTWRITEM();
  ADDWRITEM("P", char);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&prob, double);

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
  if(!SENDWRITEM()) {
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


  if(PG_ARGISNULL(1))
    info1=0;
  if(PG_ARGISNULL(2))
    info2=0;

  STARTWRITEM();
  ADDWRITEM("I", char);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&info1, int);
  ADDWRITEM(&info2, int);

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
  if(!SENDWRITEM()) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type I)");
  }
  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_extra);
Datum set_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *data = PG_GETARG_TEXT_P(1);
  char *str=text_to_cstring(data);
  unsigned len=strlen(str);

  STARTWRITEM();
  ADDWRITEM("E", char);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&len, unsigned);

  assert(PIPE_BUF-bufferpos>len);
  memcpy(buffer+bufferpos, str, len), bufferpos+=len;
  pfree(str);

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
  if(!SENDWRITEM()) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type E)");
  }
  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_extra);
Datum get_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *result;
  unsigned len;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("e", char);
  ADDWRITEM(token, pg_uuid_t);

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!SENDWRITEM() || !READB(len, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate with pipe (message type e)");
  }

  result = palloc(len + VARHDRSZ);
  SET_VARSIZE(result, VARHDRSZ + len);

  if(read(provsql_shared_state->pipembr, VARDATA(result), len)<len) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate with pipe (message type e)");
  }

  LWLockRelease(provsql_shared_state->lock);

  PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(get_nb_gates);
Datum get_nb_gates(PG_FUNCTION_ARGS)
{
  unsigned nb;

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("n", char) || !READB(nb, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate with pipe (message type n)");
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

  STARTWRITEM();
  ADDWRITEM("c", char);
  ADDWRITEM(token, pg_uuid_t);

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!SENDWRITEM()) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type c)");
  }

  if(!READB(nb_children, unsigned)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type c)");
  }

  children_ptr = palloc(nb_children * sizeof(Datum));
  children=calloc(nb_children, sizeof(pg_uuid_t));
  if(read(provsql_shared_state->pipembr, children, nb_children*sizeof(pg_uuid_t))<nb_children*sizeof(pg_uuid_t)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type c)");
  }
  LWLockRelease(provsql_shared_state->lock);

  for(unsigned i=0; i<nb_children; ++i)
    children_ptr[i] = UUIDPGetDatum(&children[i]);

  constants=get_constants(true);
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

  STARTWRITEM();
  ADDWRITEM("p", char);
  ADDWRITEM(token, pg_uuid_t);

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!SENDWRITEM() || !READB(result, double)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate with pipe (message type p)");
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

  STARTWRITEM();
  ADDWRITEM("i", char);
  ADDWRITEM(token, pg_uuid_t);

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!SENDWRITEM() || !READB(info1, int) || !READB(info2, int)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot communicate with pipe (message type i)");
  }

  LWLockRelease(provsql_shared_state->lock);

  if(info1 == 0 && info2 == 0)
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
