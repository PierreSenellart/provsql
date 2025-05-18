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

#include "circuit_cache.h"

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
  gate_type type;
  constants_t constants=get_constants(true);

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  type = circuit_cache_get_type(*token);
  if(type!=gate_invalid)
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]); ;

  STARTWRITEM();
  ADDWRITEM("t", char);
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(type, gate_type)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate on pipe (message type t)");
  }

  provsql_shmem_unlock();

  if(type==gate_invalid)
    PG_RETURN_NULL();
  else {
    circuit_cache_create_gate(*token, type, 0, NULL);
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

  if(circuit_cache_create_gate(*token, type, nb_children, children_data))
    PG_RETURN_VOID();

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

    provsql_shmem_lock_shared();
    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot write to pipe (message type C)");
    }
    provsql_shmem_unlock();
  } else {
    // Not enough space in buffer, pipe write won't be atomic, we need to
    // make several writes and use locks
    unsigned children_per_batch = PIPE_BUF/sizeof(pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot write to pipe (message type C)");
    }

    for(unsigned j=0; j<1+(nb_children-1)/children_per_batch; ++j) {
      STARTWRITEM();

      for(unsigned i=j*children_per_batch; i<(j+1)*children_per_batch && i<nb_children; ++i) {
        ADDWRITEM(&children_data[i], pg_uuid_t);
      }

      if(!SENDWRITEM()) {
        provsql_shmem_unlock();
        elog(ERROR, "Cannot write to pipe (message type C)");
      }
    }

    provsql_shmem_unlock();
  }

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob);
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);
  char result;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_prob");

  STARTWRITEM();
  ADDWRITEM("P", char);
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&prob, double);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM() || !READB(result, char)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot write to pipe");
  }
  provsql_shmem_unlock();

  if(!result)
    elog(ERROR, "set_prob called on non-input gate");

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
  ADDWRITEM(&info1, unsigned);
  ADDWRITEM(&info2, unsigned);

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot write to pipe (message type I)");
  }
  provsql_shmem_unlock();

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

  provsql_shmem_lock_shared();
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot write to pipe (message type E)");
  }
  provsql_shmem_unlock();

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

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(len, unsigned)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate with pipe (message type e)");
  }

  result = palloc(len + VARHDRSZ);
  SET_VARSIZE(result, VARHDRSZ + len);

  if(read(provsql_shared_state->pipembr, VARDATA(result), len)<len) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate with pipe (message type e)");
  }

  provsql_shmem_unlock();

  PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(get_nb_gates);
Datum get_nb_gates(PG_FUNCTION_ARGS)
{
  unsigned long nb;

  provsql_shmem_lock_exclusive();

  if(!WRITEM("n", char) || !READB(nb, unsigned long)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate with pipe (message type n)");
  }

  provsql_shmem_unlock();

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

  nb_children = circuit_cache_get_children(*token, &children);

  if(!children) {
    STARTWRITEM();
    ADDWRITEM("c", char);
    ADDWRITEM(token, pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot write to pipe (message type c)");
    }

    if(!READB(nb_children, unsigned)) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot read response from pipe (message type c)");
    }

    children=calloc(nb_children, sizeof(pg_uuid_t));

    {
      char *p = (char*)children;
      ssize_t actual_read, remaining_size=nb_children*sizeof(pg_uuid_t);
      while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
        if(actual_read<=0) {
          provsql_shmem_unlock();
          elog(ERROR, "Cannot read from pipe (message type c)");
        } else {
          remaining_size-=actual_read;
          p+=actual_read;
        }
      }
    }
    provsql_shmem_unlock();

    circuit_cache_create_gate(*token, gate_invalid, nb_children, children);
  }

  children_ptr = palloc(nb_children * sizeof(Datum));
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

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(result, double)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate with pipe (message type p)");
  }

  provsql_shmem_unlock();

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

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(info1, int) || !READB(info2, int)) {
    provsql_shmem_unlock();
    elog(ERROR, "Cannot communicate with pipe (message type i)");
  }

  provsql_shmem_unlock();

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
