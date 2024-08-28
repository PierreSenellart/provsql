#include <unistd.h>

#include "postgres.h"
#include "storage/shmem.h"

#include "provsql_shmem.h"
#include "provsql_mmap.h"

shmem_startup_hook_type prev_shmem_startup = NULL;
#if (PG_VERSION_NUM >= 150000)
shmem_request_hook_type prev_shmem_request = NULL;
#endif

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

void provsql_shmem_request(void)
{
#if (PG_VERSION_NUM >= 150000)
  if (prev_shmem_request)
    prev_shmem_request();
#endif

  RequestAddinShmemSpace(provsql_memsize());

  RequestNamedLWLockTranche("provsql", 1);
}

void provsql_shmem_lock_exclusive(void)
{
  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);
}

void provsql_shmem_lock_shared(void)
{
  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
}

void provsql_shmem_unlock(void)
{
  LWLockRelease(provsql_shared_state->lock);
}
