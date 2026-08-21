/**
 * @file provsql_mmap.c
 * @brief Background worker registration and IPC primitives for mmap-backed storage.
 *
 * Implements the PostgreSQL background worker lifecycle functions declared
 * in @c provsql_mmap.h:
 * - @c RegisterProvSQLMMapWorker(): registers the worker with the postmaster
 *   during @c _PG_init().
 * - @c provsql_mmap_worker(): worker entry point; sets up signal handlers
 *   and enters @c provsql_mmap_main_loop().
 *
 * The IPC between normal backends and the background worker is handled in
 * @c MMappedCircuit.cpp.  This file provides the PostgreSQL-specific glue
 * (background worker API, signal handling).
 *
 * Also declares the shared write buffer @c buffer[] and position counter
 * @c bufferpos used by the @c STARTWRITEM / @c ADDWRITEM / @c SENDWRITEM
 * macros in @c provsql_mmap.h.
 *
 * The gate-creation SQL functions (e.g. @c create_gate()) that backends
 * call are also implemented here; they acquire the IPC lock, write a
 * message to the background worker, and wait for an acknowledgment.
 */
#include "provsql_mmap.h"
#include "provsql_rmgr.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <math.h>
#include <assert.h>

#include "postgres.h"
#include "access/xact.h"
#include "postmaster/bgworker.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

#include "circuit_cache.h"

#ifdef PROVSQL_INPROCESS_STORE

char *buffer = NULL; // flawfinder: ignore
unsigned bufferpos = 0;
size_t buffercap = 0;

void provsql_buffer_ensure(size_t need)
{
  if(need > buffercap) {
    size_t newcap = buffercap ? buffercap * 2 : 4096;
    while(newcap < need)
      newcap *= 2;
    buffer = realloc(buffer, newcap);
    if(!buffer)
      provsql_error("ProvSQL: out of memory growing the IPC buffer");
    buffercap = newcap;
  }
}

/* No background worker in the single-process build. */

#else

char buffer[PIPE_BUF]={}; // flawfinder: ignore
unsigned bufferpos=0;

bool provsql_read_all(int fd, void *dst, size_t n)
{
  char *p = dst;
  size_t remaining = n;
  while(remaining > 0) {
    ssize_t r = read(fd, p, remaining); // flawfinder: ignore
    if(r <= 0)
      return false;
    remaining -= r;
    p += r;
  }
  return true;
}

#if PG_VERSION_NUM >= 190000
/* PostgreSQL 19 changed the default background-worker SIGTERM handler
 * from bgworker_die() (immediate FATAL from the signal handler) to the
 * flag-based die(), which only acts at the next CHECK_FOR_INTERRUPTS().
 * This worker blocks in read() on the IPC pipe (restarted by
 * SA_RESTART), so it would never observe the flag and a fast shutdown
 * would hang on it.  Restore the pre-19 semantics: the worker holds no
 * transaction state, and being interrupted between messages leaves the
 * store consistent, so exiting mid-read is fine.  (Being interrupted
 * *during* a write is what the write ordering in MMappedCircuit.cpp is
 * there for; provsql.check_store() reports what an interruption left
 * behind.) */
static void provsql_worker_die(SIGNAL_ARGS)
{
  ereport(FATAL,
          (errcode(ERRCODE_ADMIN_SHUTDOWN),
           errmsg("terminating background worker \"%s\" due to administrator command",
                  MyBgworkerEntry->bgw_type)));
}
#endif

PGDLLEXPORT void provsql_mmap_worker(Datum ignored)
{
#if PG_VERSION_NUM >= 190000
  pqsignal(SIGTERM, provsql_worker_die);
#endif
  BackgroundWorkerUnblockSignals();
  initialize_provsql_mmap();
  close(provsql_shared_state->pipebmw);
  close(provsql_shared_state->pipembr);
  provsql_log("%s initialized", MyBgworkerEntry->bgw_name);

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

#endif /* PROVSQL_INPROCESS_STORE */

/* -------------------------------------------------------------------------
 * Durability of the store across a machine crash
 *
 * A backend's writes reach the worker over a FIFO pipe and the worker
 * applies them to the mmap files, but nothing forces those files to disk
 * at the moment the writing transaction commits: the heap's WAL record is
 * fsynced, the circuit's bytes are not.  After a crash of the machine
 * (PostgreSQL crashing is harmless -- the page cache outlives it) a
 * committed row can reference a gate that never reached the disk, and an
 * unknown token reads back as an input gate, so the loss is silent.
 *
 * Two things narrow that window.  The worker forces the files out shortly
 * after the last write (PROVSQL_STORE_FLUSH_INTERVAL_MS), which bounds the
 * loss.  And, when provsql.synchronous_commit is on, a transaction that
 * wrote to the store sends a sync request before it commits and waits for
 * the reply, which closes the window entirely: the reply comes after every
 * earlier message of this backend has been applied and forced.
 * ------------------------------------------------------------------------- */

bool provsql_synchronous_commit = false;

/** Whether the current transaction has written anything to the store. */
static bool store_written = false;
static bool store_callbacks_registered = false;

/** @brief Send the sync barrier and wait for the worker's acknowledgement. */
static void provsql_store_sync_barrier(void)
{
  char ack;

  STARTWRITEM();
  ADDWRITEM("S", char);
  ADDWRITEDB();

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM() || !READB(ack, char)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type S)");
  }
  provsql_shmem_unlock();
}

static void provsql_store_xact_callback(XactEvent event, void *arg)
{
  (void) arg;
  switch(event) {
  case XACT_EVENT_PRE_COMMIT:
  case XACT_EVENT_PRE_PREPARE:
    /* Still inside the transaction, so raising here aborts the commit
       rather than leaving it half-durable. */
    if(store_written && provsql_synchronous_commit)
      provsql_store_sync_barrier();
    break;
  case XACT_EVENT_COMMIT:
  case XACT_EVENT_ABORT:
  case XACT_EVENT_PREPARE:
  case XACT_EVENT_PARALLEL_COMMIT:
  case XACT_EVENT_PARALLEL_ABORT:
    store_written = false;
    break;
  default:
    break;
  }
}

/** @brief What every store mutation does before it reaches the pipe:
 *  refuse it on a standby, and write it to the WAL.  @p data is the
 *  complete message, opcode first. */
static void provsql_log_store_write(const char *data, size_t len)
{
  if(!provsql_store_write_allowed())
    ereport(ERROR,
            (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
             errmsg("cannot write to the ProvSQL circuit store during recovery"),
             errdetail("The store is maintained on a standby by replaying the "
                       "primary's records; a backend writing to it would make "
                       "the two diverge."),
             errhint("Provenance queries create gates as they run, reads "
                     "included, so they only work on the primary.")));
  provsql_wal_log_store_message(data, len);
}

/** @brief The same, for a mutation made by a live transaction: it also
 *  arms the at-commit sync barrier. */
static void provsql_before_store_write(const char *data, size_t len)
{
  provsql_log_store_write(data, len);
  provsql_store_note_write();
}

void provsql_store_note_write(void)
{
  if(!store_callbacks_registered) {
    RegisterXactCallback(provsql_store_xact_callback, NULL);
    store_callbacks_registered = true;
  }
  store_written = true;
}

bool provsql_store_written(void)
{
  return store_written;
}

void provsql_circuit_cleanup_request(bool dry_run, const pg_uuid_t *roots,
                                     int64 nb_roots,
                                     provsql_cleanup_result *out)
{
  char flag = dry_run ? 1 : 0;
  unsigned long n = (unsigned long) nb_roots;

  /* The root set is as large as the number of distinct tokens stored in
     the database, so it does not fit one atomic pipe write.  The lock is
     held across the whole exchange, which is what makes the sequence of
     writes one message; nothing else can be talking to the worker anyway,
     since the caller holds the database exclusively. */
  provsql_shmem_lock_exclusive();

  STARTWRITEM();
  ADDWRITEM("X", char);
  ADDWRITEDB();
  ADDWRITEM(&flag, char);
  ADDWRITEM(&n, unsigned long);
  if(!SENDWRITEM()) {
    provsql_shmem_unlock();
    provsql_error("Cannot write to pipe (message type X)");
  }

#ifdef PROVSQL_INPROCESS_STORE
  /* The in-memory FIFO has no atomicity limit and the dispatch runs
     inside SENDWRITEM, so the roots must accompany the header. */
  provsql_shmem_unlock();
  provsql_error("circuit_cleanup is not available in the single-process build");
#else
  {
    unsigned long per_batch = PIPE_BUF / sizeof(pg_uuid_t);
    for(unsigned long i = 0; i < n; ) {
      unsigned long j;
      STARTWRITEM();
      for(j = 0; j < per_batch && i < n; ++j, ++i)
        ADDWRITEM(&roots[i], pg_uuid_t);
      if(!SENDWRITEM()) {
        provsql_shmem_unlock();
        provsql_error("Cannot write to pipe (message type X)");
      }
    }
  }

  if(!READB(out->gates_before, uint64) || !READB(out->gates_after, uint64)
     || !READB(out->wires_before, uint64) || !READB(out->wires_after, uint64)
     || !READB(out->extra_before, uint64) || !READB(out->extra_after, uint64)) {
    provsql_shmem_unlock();
    provsql_error("Cannot read response from pipe (message type X)");
  }
  provsql_shmem_unlock();
#endif
}

void provsql_replay_store_message(const char *data, size_t len)
{
#ifdef PROVSQL_INPROCESS_STORE
  (void) data; (void) len;
#else
  const char *p = data;
  size_t left = len;

  if(len == 0)
    return;

  /* The worker is the single writer, in recovery as in normal running:
     the message goes back down the same pipe a backend would use.  The
     lock is held across the whole write so the (possibly chunked)
     message stays one message. */
  provsql_shmem_lock_exclusive();

  while(left > 0) {
    size_t chunk = left > PIPE_BUF ? PIPE_BUF : left;
    if(write(provsql_shared_state->pipebmw, p, chunk) == -1) {
      provsql_shmem_unlock();
      provsql_error("Cannot replay a store message to the pipe");
    }
    p += chunk;
    left -= chunk;
  }

  /* Opcodes that answer must be drained, or the reply would be read as
     the answer to somebody else's later question. */
  if(data[0] == 'P') {
    char result;
    double stored;
    if(!READB(result, char) || !READB(stored, double)) {
      provsql_shmem_unlock();
      provsql_error("Cannot read the reply to a replayed store message");
    }
  }

  provsql_shmem_unlock();
#endif
}

PG_FUNCTION_INFO_V1(check_store);
/**
 * @brief Report what does not add up in this database's circuit store.
 *
 * A store nothing has damaged answers zero to every count.  A non-zero
 * one means a write was interrupted at a point the ordering rules do not
 * cover, or that a set of files was copied at different instants -- a
 * file-level backup of a running server, or a base backup.
 * @c provsql.circuit_cleanup() rebuilds the store from what is still
 * reachable.
 */
Datum check_store(PG_FUNCTION_ARGS)
{
  char unclean;
  unsigned long nb_gates, nb_mapping, next_value,
                dangling, unreferenced, bad_wires, bad_extra;
  TupleDesc tupdesc;
  Datum values[8];
  bool nulls[8] = {false, false, false, false, false, false, false, false};

  STARTWRITEM();
  ADDWRITEM("k", char);
  ADDWRITEDB();

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM()
     || !READB(unclean, char)
     || !READB(nb_gates, unsigned long)
     || !READB(nb_mapping, unsigned long)
     || !READB(next_value, unsigned long)
     || !READB(dangling, unsigned long)
     || !READB(unreferenced, unsigned long)
     || !READB(bad_wires, unsigned long)
     || !READB(bad_extra, unsigned long)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type k)");
  }
  provsql_shmem_unlock();

  if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    provsql_error("check_store: expected composite return type");
  tupdesc = BlessTupleDesc(tupdesc);

  values[0] = BoolGetDatum(unclean != 0);
  values[1] = Int64GetDatum((int64) nb_gates);
  values[2] = Int64GetDatum((int64) nb_mapping);
  values[3] = Int64GetDatum((int64) next_value);
  values[4] = Int64GetDatum((int64) dangling);
  values[5] = Int64GetDatum((int64) unreferenced);
  values[6] = Int64GetDatum((int64) bad_wires);
  values[7] = Int64GetDatum((int64) bad_extra);

  PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

PG_FUNCTION_INFO_V1(get_gate_type);
/** @brief PostgreSQL-callable wrapper for get_gate_type().
 *
 * On cache miss this fetches BOTH the gate type and its children from
 * the worker, in one critical section, then caches them together. If
 * we cached only the type (with an empty children list), a subsequent
 * get_children() call for the same token would consult the cache, find
 * the entry, and return 0 children : never querying the worker for the
 * real children. provsql.provenance_evaluate hits exactly that pattern
 * (it calls get_gate_type first, then unnest(get_children(...))) and
 * silently folds plus/times gates over an empty set.
 */
/** @brief Fetch a gate's type and children, cache-first with a worker
 *  round-trip (and cache fill) on a miss.  Factored out of the
 *  get_gate_type() wrapper for in-extension callers that walk the circuit
 *  from C (e.g. the annotation-transparent set_prob()).  On return
 *  @p *children_out is a @c calloc'd array to be freed by the caller, or
 *  @c NULL when the gate has no children. */
gate_type provsql_fetch_gate(const pg_uuid_t *token,
                             unsigned *nb_children_out,
                             pg_uuid_t **children_out)
{
  gate_type type;
  unsigned nb_children = 0;
  pg_uuid_t *children = NULL;

  type = circuit_cache_get_type(*token);
  if(type!=gate_invalid) {
    *nb_children_out = circuit_cache_get_children(*token, children_out);
    return type;
  }

  /* Type fetch (message 't'). */
  STARTWRITEM();
  ADDWRITEM("t", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(type, gate_type)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate on pipe (message type t)");
  }

  /* Children fetch (message 'c'), batched in the same critical
   * section so the cache entry below is complete. Skipped when the
   * token is unknown (worker reports gate_invalid). */
  if(type != gate_invalid) {
    STARTWRITEM();
    ADDWRITEM("c", char);
    ADDWRITEDB();
    ADDWRITEM(token, pg_uuid_t);

    if(!SENDWRITEM() || !READB(nb_children, unsigned)) {
      provsql_shmem_unlock();
      provsql_error("Cannot communicate on pipe (message type c during get_gate_type)");
    }

    if(nb_children > 0) {
      children = calloc(nb_children, sizeof(pg_uuid_t));
      if(!READB_BYTES(children, nb_children * sizeof(pg_uuid_t))) {
        provsql_shmem_unlock();
        provsql_error("Cannot read children from pipe (during get_gate_type)");
      }
    }
  }

  provsql_shmem_unlock();

  /* Skip caching the gate_input lazy default: MMappedCircuit::getGateType
   * returns gate_input both for real input gates and for tokens that are
   * not yet in the mapping. Caching the latter would poison subsequent
   * create_gate() calls in this session (the cache hit would short-circuit
   * the worker IPC, dropping the gate). The cost is one extra IPC per
   * lookup of a real input gate -- acceptable. */
  if(!(type == gate_input && nb_children == 0))
    circuit_cache_create_gate(*token, type, nb_children, children);
  *nb_children_out = nb_children;
  *children_out = children;
  return type;
}

Datum get_gate_type(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type;
  constants_t constants=get_constants(true);
  unsigned nb_children = 0;
  pg_uuid_t *children = NULL;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  type = provsql_fetch_gate(token, &nb_children, &children);
  if(children) free(children);
  PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[type]);
}

/** @brief Internal entry point behind create_gate(): cache + worker IPC.
 *
 * Factored out of the SQL-callable wrapper so in-extension C/C++ code
 * (e.g. the decomposition-aligned reachability materialiser) can create
 * gates without Datum marshalling or gate-type-OID lookups.  Same
 * semantics: write-through to the per-session cache, then the C message
 * to the background worker; MMappedCircuit::createGate is idempotent on
 * already-mapped tokens. */
void provsql_internal_create_gate(const pg_uuid_t *token, gate_type type,
                                  unsigned nb_children,
                                  const pg_uuid_t *children_data)
{
  /* Populate the per-session cache, but unconditionally fall through to
   * the worker IPC: a cache hit only proves "this token has been seen
   * in this session before" (e.g. by get_gate_type returning the
   * gate_input lazy default for an unknown token) -- not "the worker
   * already has a gate for it". Skipping the IPC on a cache hit caused
   * silently-dropped create_gate calls under concurrent backends.
   * MMappedCircuit::createGate is idempotent on already-mapped tokens. */
  circuit_cache_create_gate(*token, type, nb_children, children_data);

  /* The WAL record is the whole logical message, children included, even
     though the pipe may need several writes for it. */
  {
    size_t header = sizeof(char) + 2 * sizeof(Oid) + sizeof(pg_uuid_t)
                    + sizeof(gate_type) + sizeof(unsigned);
    size_t len = header + nb_children * sizeof(pg_uuid_t);
    char *msg = palloc(len);
    char *p = msg;
    *p++ = 'C';
    memcpy(p, &MyDatabaseId, sizeof(Oid)); p += sizeof(Oid);
    memcpy(p, &MyDatabaseTableSpace, sizeof(Oid)); p += sizeof(Oid);
    memcpy(p, token, sizeof(pg_uuid_t)); p += sizeof(pg_uuid_t);
    memcpy(p, &type, sizeof(gate_type)); p += sizeof(gate_type);
    memcpy(p, &nb_children, sizeof(unsigned)); p += sizeof(unsigned);
    for(unsigned i=0; i<nb_children; ++i) {
      memcpy(p, &children_data[i], sizeof(pg_uuid_t));
      p += sizeof(pg_uuid_t);
    }
    provsql_before_store_write(msg, len);
    pfree(msg);
  }

  STARTWRITEM();
  ADDWRITEM("C", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&type, gate_type);
  ADDWRITEM(&nb_children, unsigned);

#ifdef PROVSQL_INPROCESS_STORE
  /* The in-memory FIFO has no PIPE_BUF atomicity limit: always send the
     gate and all its children as a single message. */
  if(1) {
#else
  if(PIPE_BUF-bufferpos>nb_children*sizeof(pg_uuid_t)) {
#endif
    // Enough space in the buffer for an atomic write, no need of
    // exclusive locks

    for(unsigned i=0; i<nb_children; ++i)
      ADDWRITEM(&children_data[i], pg_uuid_t);

    provsql_shmem_lock_shared();
    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type C)");
    }
    provsql_shmem_unlock();
  }
#ifndef PROVSQL_INPROCESS_STORE
  else {
    // Not enough space in buffer, pipe write won't be atomic, we need to
    // make several writes and use locks
    unsigned children_per_batch = PIPE_BUF/sizeof(pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type C)");
    }

    for(unsigned j=0; j<1+(nb_children-1)/children_per_batch; ++j) {
      STARTWRITEM();

      for(unsigned i=j*children_per_batch; i<(j+1)*children_per_batch && i<nb_children; ++i) {
        ADDWRITEM(&children_data[i], pg_uuid_t);
      }

      if(!SENDWRITEM()) {
        provsql_shmem_unlock();
        provsql_error("Cannot write to pipe (message type C)");
      }
    }

    provsql_shmem_unlock();
  }
#endif
}

/** @brief Send a probability write and read back what the store made of
 *  it.  @p tracked arms the at-commit sync barrier; the one caller that
 *  passes false is the rollback path, which runs when the transaction
 *  that would have committed is already gone. */
static provsql_set_prob_result provsql_send_set_prob(const pg_uuid_t *token,
                                                     double prob,
                                                     double *existing,
                                                     bool tracked)
{
  char result;
  double stored;

  STARTWRITEM();
  ADDWRITEM("P", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&prob, double);
  if(tracked)
    provsql_before_store_write(buffer, bufferpos);
  else
    provsql_log_store_write(buffer, bufferpos);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM() || !READB(result, char) || !READB(stored, double)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type P)");
  }
  provsql_shmem_unlock();

  if(existing)
    *existing = stored;
  return (provsql_set_prob_result) result;
}

provsql_set_prob_result provsql_internal_set_prob(const pg_uuid_t *token,
                                                  double prob,
                                                  double *existing)
{
  return provsql_send_set_prob(token, prob, existing, true);
}

void provsql_internal_clear_prob(const pg_uuid_t *token)
{
  provsql_send_set_prob(token, NAN, NULL, false);
}

bool provsql_internal_get_prob_written(const pg_uuid_t *token, double *prob)
{
  char has;
  double stored;

  STARTWRITEM();
  ADDWRITEM("q", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM() || !READB(has, char) || !READB(stored, double)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type q)");
  }
  provsql_shmem_unlock();

  if(prob)
    *prob = stored;
  return has != 0;
}

/** @brief Internal entry point behind set_infos(): worker IPC only. */
void provsql_internal_set_infos(const pg_uuid_t *token, unsigned info1,
                                unsigned info2)
{
  char result;
  unsigned had1, had2;

  STARTWRITEM();
  ADDWRITEM("I", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&info1, unsigned);
  ADDWRITEM(&info2, unsigned);
  provsql_before_store_write(buffer, bufferpos);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM() || !READB(result, char) || !READB(had1, unsigned)
     || !READB(had2, unsigned)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type I)");
  }
  provsql_shmem_unlock();

  if((provsql_set_annotation_result) result == PROVSQL_SET_ANNOTATION_ALREADY_SET)
    ereport(ERROR,
            (errmsg("gate %s already records the annotation (%u, %u), "
                    "not (%u, %u)",
                    DatumGetCString(DirectFunctionCall1(
                                      uuid_out, UUIDPGetDatum((pg_uuid_t *) token))),
                    had1, had2, info1, info2),
             errdetail("A gate's annotation is written once, like the gate "
                       "itself and its probability, so that a transaction "
                       "that rolls back leaves the circuit as it found it.")));
}

PG_FUNCTION_INFO_V1(create_gate);
/** @brief PostgreSQL-callable wrapper for create_gate(). */
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
    provsql_error("Invalid NULL value passed to create_gate");

  if(children) {
    if(ARR_NDIM(children) > 1)
      provsql_error("Invalid multi-dimensional array passed to create_gate");
    if(array_contains_nulls(children))
      provsql_error("create_gate: children array must not contain NULL "
                    "elements (filter them out before calling)");
    if(ARR_NDIM(children) == 1)
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
    provsql_error("Invalid gate type");
  }

  if(nb_children>0)
    children_data = (pg_uuid_t*) ARR_DATA_PTR(children);
  else
    children_data = NULL;

  provsql_internal_create_gate(token, type, nb_children, children_data);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_infos);
/** @brief PostgreSQL-callable wrapper for set_infos(). */
Datum set_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 = PG_GETARG_INT32(1);
  unsigned info2 = PG_GETARG_INT32(2);


  if(PG_ARGISNULL(1))
    info1=0;
  if(PG_ARGISNULL(2))
    info2=0;

  provsql_internal_set_infos(token, info1, info2);

  PG_RETURN_VOID();
}

/** @brief Internal entry point behind set_extra(): worker IPC only. */
void provsql_internal_set_extra(const pg_uuid_t *token, const char *str)
{
  unsigned len=strlen(str);
  char result;
  unsigned had_len = 0;
  char *had = NULL;

  STARTWRITEM();
  ADDWRITEM("E", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);
  ADDWRITEM(&len, unsigned);

#ifdef PROVSQL_INPROCESS_STORE
  provsql_buffer_ensure(bufferpos+len);
#else
  assert(PIPE_BUF-bufferpos>len);
#endif
  memcpy(buffer+bufferpos, str, len), bufferpos+=len;
  provsql_before_store_write(buffer, bufferpos);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM() || !READB(result, char) || !READB(had_len, unsigned)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type E)");
  }
  if(had_len > 0) {
    had = palloc(had_len + 1);
    if(!READB_BYTES(had, had_len)) {
      provsql_shmem_unlock();
      provsql_error("Cannot communicate with pipe (message type E)");
    }
    had[had_len] = '\0';
  }
  provsql_shmem_unlock();

  if((provsql_set_annotation_result) result == PROVSQL_SET_ANNOTATION_ALREADY_SET)
    ereport(ERROR,
            (errmsg("gate %s already records the annotation \"%s\", not \"%s\"",
                    DatumGetCString(DirectFunctionCall1(
                                      uuid_out, UUIDPGetDatum((pg_uuid_t *) token))),
                    had ? had : "", str),
             errdetail("A gate's annotation is written once, like the gate "
                       "itself and its probability, so that a transaction "
                       "that rolls back leaves the circuit as it found it.")));
}

PG_FUNCTION_INFO_V1(set_extra);
/** @brief PostgreSQL-callable wrapper for set_extra(). */
Datum set_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *data = PG_GETARG_TEXT_P(1);
  char *str=text_to_cstring(data);

  provsql_internal_set_extra(token, str);
  pfree(str);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_extra);
/** @brief PostgreSQL-callable wrapper for get_extra(). */
Datum get_extra(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  text *result;
  unsigned len;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("e", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(len, unsigned)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type e)");
  }

  result = palloc(len + VARHDRSZ);
  SET_VARSIZE(result, VARHDRSZ + len);

  if(!READB_BYTES(VARDATA(result), len)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type e)");
  }

  provsql_shmem_unlock();

  PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(get_nb_gates);
/** @brief PostgreSQL-callable wrapper for get_nb_gates(). */
Datum get_nb_gates(PG_FUNCTION_ARGS)
{
  unsigned long nb;

  STARTWRITEM();
  ADDWRITEM("n", char);
  ADDWRITEDB();

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(nb, unsigned long)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type n)");
  }

  provsql_shmem_unlock();

  PG_RETURN_INT64((long) nb);
}

PG_FUNCTION_INFO_V1(get_children);
/** @brief PostgreSQL-callable wrapper for get_children(). */
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
    ADDWRITEDB();
    ADDWRITEM(token, pg_uuid_t);

    provsql_shmem_lock_exclusive();

    if(!SENDWRITEM()) {
      provsql_shmem_unlock();
      provsql_error("Cannot write to pipe (message type c)");
    }

    if(!READB(nb_children, unsigned)) {
      provsql_shmem_unlock();
      provsql_error("Cannot read response from pipe (message type c)");
    }

    children=calloc(nb_children, sizeof(pg_uuid_t));

    if(!READB_BYTES(children, nb_children*sizeof(pg_uuid_t))) {
      provsql_shmem_unlock();
      provsql_error("Cannot read from pipe (message type c)");
    }
    provsql_shmem_unlock();

    /* Skip caching when the worker reports zero children: we cannot
     * distinguish a real zero-child gate (input/zero/one/...) from a
     * token unknown to the worker, and caching the latter poisons
     * subsequent create_gate() calls in this session. */
    if(nb_children > 0)
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
/** @brief PostgreSQL-callable wrapper for get_prob(). */
Datum get_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double result;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("p", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(result, double)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type p)");
  }

  provsql_shmem_unlock();

  if(isnan(result))
    PG_RETURN_NULL();
  else
    PG_RETURN_FLOAT8(result);
}

PG_FUNCTION_INFO_V1(get_infos);
/** @brief PostgreSQL-callable wrapper for get_infos(). */
Datum get_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 =0, info2 = 0;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  STARTWRITEM();
  ADDWRITEM("i", char);
  ADDWRITEDB();
  ADDWRITEM(token, pg_uuid_t);

  provsql_shmem_lock_exclusive();

  if(!SENDWRITEM() || !READB(info1, int) || !READB(info2, int)) {
    provsql_shmem_unlock();
    provsql_error("Cannot communicate with pipe (message type i)");
  }

  provsql_shmem_unlock();

  {
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
