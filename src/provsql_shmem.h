#ifndef PROVSQL_SHMEM_H
#define PROVSQL_SHMEM_H

#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/hsearch.h"

#include "provsql_utils.h"

extern shmem_startup_hook_type prev_shmem_startup;
#if (PG_VERSION_NUM >= 150000)
extern shmem_request_hook_type prev_shmem_request;
#endif
extern int provsql_init_nb_gates;
extern int provsql_max_nb_gates;
extern int provsql_avg_nb_wires;

uint32 provsql_hash_uuid(const void *key, Size s);
void provsql_shmem_startup(void);
Size provsql_memsize(void);
void provsql_shmem_request(void);

typedef struct provsqlSharedState
{
  LWLock *lock; // only send one message at a time
  long pipebmr, pipebmw, pipembr, pipembw;
} provsqlSharedState;
extern provsqlSharedState *provsql_shared_state;

#endif /* ifndef PROVSQL_SHMEM_H */
