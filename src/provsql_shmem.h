/**
 * @file provsql_shmem.h
 * @brief Shared-memory segment and inter-process pipe management.
 *
 * ProvSQL uses a small PostgreSQL shared-memory segment (@c provsqlSharedState)
 * to hold the file descriptors of the anonymous pipes that connect
 * normal backends to the mmap background worker, together with a
 * lightweight lock that serialises concurrent gate-creation requests.
 *
 * This header declares:
 * - @c provsqlSharedState, the layout of the shared segment.
 * - Hook functions (@c provsql_shmem_startup, @c provsql_shmem_request)
 *   that integrate with PostgreSQL's shared-memory lifecycle.
 * - Convenience wrappers (@c provsql_shmem_lock_exclusive, etc.) around
 *   the embedded @c LWLock.
 * - Saved pointers to the previous hook functions so that ProvSQL can
 *   chain into an existing hook chain.
 */
#ifndef PROVSQL_SHMEM_H
#define PROVSQL_SHMEM_H

#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"

#include "provsql_config.h"

/** @brief Saved pointer to the previous @c shmem_startup_hook, for chaining. */
extern shmem_startup_hook_type prev_shmem_startup;
#if (PG_VERSION_NUM >= 150000) || defined(DOXYGEN)
/** @brief Saved pointer to the previous @c shmem_request_hook (PG ≥ 15), for chaining. */
extern shmem_request_hook_type prev_shmem_request;
#endif

/**
 * @brief Initialise the ProvSQL shared-memory segment.
 *
 * Called from the @c shmem_startup_hook.  Creates (or attaches to) the
 * @c provsqlSharedState segment and initialises the embedded @c LWLock
 * and pipe file descriptors.  Chains to @c prev_shmem_startup if set.
 */
void provsql_shmem_startup(void);

/**
 * @brief Return the number of bytes required for the shared-memory segment.
 *
 * Used by PostgreSQL's shared-memory allocator during startup.
 *
 * @return Size in bytes of the @c provsqlSharedState structure.
 */
Size provsql_memsize(void);

/**
 * @brief Request shared memory from PostgreSQL (PG ≥ 15).
 *
 * Called from the @c shmem_request_hook (introduced in PostgreSQL 15) to
 * reserve the segment before shared memory is finalised.  Chains to
 * @c prev_shmem_request if set.  On older PostgreSQL versions this is a
 * no-op because memory reservation happened in @c _PG_init().
 */
void provsql_shmem_request(void);

/**
 * @brief Shared state stored in the PostgreSQL shared-memory segment.
 *
 * All backends and the background worker access this structure through
 * the @c provsql_shared_state global pointer.
 *
 * @c lock serialises gate-creation messages so that only one backend
 * writes to the pipe at a time.  The four @c long fields hold OS file
 * descriptors for the two anonymous pipes:
 * - @c pipebmr / @c pipebmw: requests, backend → worker (backends write)
 * - @c pipembr / @c pipembw: replies, worker → backend (worker writes)
 */
#ifdef PROVSQL_INPROCESS_STORE

/**
 * @brief Growable byte FIFO backing the in-process request/response channel.
 *
 * Replaces the inter-process pipes when there is a single PostgreSQL
 * process (browser/WASM target).  Bytes are appended at @c tail and
 * consumed from @c head; the buffer is reclaimed (head reset to 0) once
 * fully drained and grown on demand.
 */
typedef struct provsql_fifo
{
  unsigned char *buf;     ///< Backing storage (NULL until first push)
  size_t head;            ///< Read offset
  size_t tail;            ///< Write offset (== valid bytes once head is 0)
  size_t cap;             ///< Allocated capacity
} provsql_fifo;

/** @brief Append @p n bytes from @p src to @p f, growing as needed. */
bool provsql_fifo_push(provsql_fifo *f, const void *src, size_t n);
/** @brief Pop @p n bytes from @p f into @p dst; @c false on underflow. */
bool provsql_fifo_pop(provsql_fifo *f, void *dst, size_t n);

typedef struct provsqlSharedState
{
  provsql_fifo req;       ///< Request channel: backend → in-process dispatch
  provsql_fifo resp;      ///< Response channel: dispatch → backend
  char kcmcp_endpoint[256]; ///< Live endpoint of the managed KCMCP server
                            ///< ("" when none).
} provsqlSharedState;

/** @brief Point @c provsql_shared_state at the process-local state. */
void provsql_inproc_init(void);

#else

typedef struct provsqlSharedState
{
  LWLock *lock;           ///< Mutual-exclusion lock for pipe writes
  long pipebmr;           ///< Backend-to-worker pipe: read end (worker reads)
  long pipebmw;           ///< Backend-to-worker pipe: write end (backends write)
  long pipembr;           ///< Worker-to-backend pipe: read end (backends read)
  long pipembw;           ///< Worker-to-backend pipe: write end (worker writes)
  char kcmcp_endpoint[256]; ///< Live endpoint of the managed KCMCP server
                            ///< ("" when none): written by the supervisor
                            ///< worker, read by the in-extension client.
} provsqlSharedState;

#endif /* PROVSQL_INPROCESS_STORE */

/** @brief Pointer to the ProvSQL shared-memory segment (set in @c provsql_shmem_startup). */
extern provsqlSharedState *provsql_shared_state;

/**
 * @brief Acquire the ProvSQL LWLock in exclusive mode.
 *
 * Callers must pair this with @c provsql_shmem_unlock().
 */
void provsql_shmem_lock_exclusive(void);

/**
 * @brief Acquire the ProvSQL LWLock in shared mode.
 *
 * Multiple backends may hold the shared lock simultaneously.
 * Callers must pair this with @c provsql_shmem_unlock().
 */
void provsql_shmem_lock_shared(void);

/**
 * @brief Release the ProvSQL LWLock.
 *
 * Must be called after @c provsql_shmem_lock_exclusive() or
 * @c provsql_shmem_lock_shared().
 */
void provsql_shmem_unlock(void);

#endif /* ifndef PROVSQL_SHMEM_H */
