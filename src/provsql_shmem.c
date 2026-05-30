/**
 * @file provsql_shmem.c
 * @brief Shared-memory segment lifecycle and LWLock management.
 *
 * Implements the functions declared in @c provsql_shmem.h:
 * - @c provsql_shmem_startup(): allocates (or attaches to) the
 *   @c provsqlSharedState segment, initialises the @c LWLock, and
 *   calls @c pipe(2) to create the two inter-process pipes.  Chains
 *   to @c prev_shmem_startup if set.
 * - @c provsql_memsize(): returns the size of the shared segment.
 * - @c provsql_shmem_request(): requests shared memory on PG ≥ 15.
 * - @c provsql_shmem_lock_exclusive() / @c provsql_shmem_lock_shared() /
 *   @c provsql_shmem_unlock(): thin wrappers around
 *   @c LWLockAcquire() / @c LWLockRelease().
 */
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

#ifdef PROVSQL_INPROCESS_STORE

/* Single-process build: the circuit store lives in this process, reached
   through two in-memory FIFOs instead of pipes + a background worker.
   There is one backend and no shared memory, so the locks are no-ops. */

static provsqlSharedState inproc_state;

void provsql_inproc_init(void)
{
  memset(&inproc_state, 0, sizeof(inproc_state));
  provsql_shared_state = &inproc_state;
}

bool provsql_fifo_push(provsql_fifo *f, const void *src, size_t n)
{
  if(f->tail + n > f->cap) {
    /* Reclaim already-consumed bytes before growing. */
    if(f->head > 0) {
      memmove(f->buf, f->buf + f->head, f->tail - f->head);
      f->tail -= f->head;
      f->head = 0;
    }
    if(f->tail + n > f->cap) {
      size_t newcap = f->cap ? f->cap * 2 : 4096;
      while(newcap < f->tail + n)
        newcap *= 2;
      f->buf = realloc(f->buf, newcap);
      if(!f->buf)
        provsql_error("ProvSQL: out of memory growing in-process FIFO");
      f->cap = newcap;
    }
  }
  memcpy(f->buf + f->tail, src, n);
  f->tail += n;
  return true;
}

bool provsql_fifo_pop(provsql_fifo *f, void *dst, size_t n)
{
  if(f->tail - f->head < n)
    return false;
  memcpy(dst, f->buf + f->head, n);
  f->head += n;
  if(f->head == f->tail)
    f->head = f->tail = 0;
  return true;
}

bool provsql_inproc_send(const char *buf, size_t len)
{
  char c;
  Oid db_oid;

  provsql_fifo_push(&provsql_shared_state->req, buf, len);

  if(!READM(c, char) || !READM(db_oid, Oid))
    return false;
  provsql_mmap_dispatch(c, db_oid);
  return true;
}

/* No shared memory and no background worker to set up. */
void provsql_shmem_startup(void) {}
Size provsql_memsize(void) { return 0; }
void provsql_shmem_request(void) {}

/* One backend: nothing to serialise. */
void provsql_shmem_lock_exclusive(void) {}
void provsql_shmem_lock_shared(void) {}
void provsql_shmem_unlock(void) {}

#else

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
    sizeof(provsqlSharedState),
    &found);

  if(!found) {
    provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
  }

  LWLockRelease(AddinShmemInitLock);

  // Already initialized
  if(found)
    return;

  if(pipe(pipes_b_to_m) || pipe(pipes_m_to_b))
    provsql_error("Cannot create pipe to communicate with MMap worker");

  provsql_shared_state->pipebmr=pipes_b_to_m[0];
  provsql_shared_state->pipebmw=pipes_b_to_m[1];
  provsql_shared_state->pipembr=pipes_m_to_b[0];
  provsql_shared_state->pipembw=pipes_m_to_b[1];
  provsql_shared_state->kcmcp_endpoint[0]='\0';
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

#endif /* PROVSQL_INPROCESS_STORE */
