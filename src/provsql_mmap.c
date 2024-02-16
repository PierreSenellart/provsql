#include "provsql_mmap.h"

#include <unistd.h>
#include "postgres.h"
#include "postmaster/bgworker.h"

void provsql_mmap_worker(Datum ignored)
{
  BackgroundWorkerUnblockSignals();

  elog(LOG, "%s initialized", MyBgworkerEntry->bgw_name);

  while(!sleep(1))
  {
  }
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
