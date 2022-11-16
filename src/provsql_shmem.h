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

void provsql_shmem_startup(void);
Size provsql_memsize(void);
void provsql_shmem_request(void);

typedef struct provsqlSharedState
{
  LWLock *lock; // protect access to the shared data
  unsigned nb_wires;
  pg_uuid_t wires[FLEXIBLE_ARRAY_MEMBER];
} provsqlSharedState;
extern provsqlSharedState *provsql_shared_state;

typedef struct provsqlHashEntry
{
  pg_uuid_t key;
  gate_type type;
  unsigned nb_children;
  unsigned children_idx;
  double prob;
  unsigned info1;
  unsigned info2;
} provsqlHashEntry;
extern HTAB *provsql_hash;

int provsql_serialize(const char*);
int provsql_deserialize(const char*);

#endif /* ifndef PROVSQL_SHMEM_H */
