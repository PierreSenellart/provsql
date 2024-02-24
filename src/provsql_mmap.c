#include "provsql_mmap.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

#include <errno.h>
#include <unistd.h>
#include <poll.h>

#include "postgres.h"
#include "postmaster/bgworker.h"
#include "fmgr.h"

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

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(!WRITEM("t", char) || !WRITEM(token, pg_uuid_t)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot write to pipe (message type t)");
  }

  if(!READB(type, gate_type)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Cannot read response from pipe (message type t)");
  }

  LWLockRelease(provsql_shared_state->lock);

  if(type==gate_invalid)
    PG_RETURN_NULL();
  else {
    constants_t constants=initialize_constants(true);
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]);
  }
}
