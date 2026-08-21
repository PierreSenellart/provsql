/**
 * @file provsql_mmap.h
 * @brief Background worker and IPC primitives for mmap-backed circuit storage.
 *
 * ProvSQL persists the provenance circuit in memory-mapped files so that
 * data survives transaction boundaries and is shared across backend
 * processes.  Because multiple backends may create gates concurrently, a
 * dedicated PostgreSQL background worker (@c provsql_mmap_worker) is the
 * sole writer to those files; normal backends communicate with it through
 * a pair of anonymous pipes described in @c provsqlSharedState.
 *
 * This header exposes:
 * - Functions to register, start, and manage the background worker.
 * - A set of pipe I/O macros (@c READM, @c READB, @c WRITEB, @c WRITEM)
 *   that wrap @c read()/@c write() calls on the inter-process pipes.
 * - A buffered-write interface (@c STARTWRITEM, @c ADDWRITEM, @c SENDWRITEM)
 *   that batches multiple fields into a single @c write() to stay within
 *   the atomic @c PIPE_BUF guarantee.
 */
#ifndef PROVSQL_MMAP_H
#define PROVSQL_MMAP_H

#include "limits.h"
#include <unistd.h>

#include "postgres.h"
#include "provsql_utils.h"
#include "provsql_config.h"

/**
 * @brief Entry point for the ProvSQL mmap background worker.
 *
 * Called by the postmaster when it launches the background worker.
 * Enters the main loop (@c provsql_mmap_main_loop()) and never returns
 * normally.  The single @c Datum argument is required by the
 * background-worker API but is not used.
 */
void provsql_mmap_worker(Datum);

/**
 * @brief Register the ProvSQL mmap background worker with PostgreSQL.
 *
 * Must be called from the extension's @c _PG_init() function so that
 * the postmaster starts the worker on the next connection.
 */
void RegisterProvSQLMMapWorker(void);

/**
 * @brief Initialise the circuit store.
 *
 * Called once by the background worker at startup.  The per-database
 * files themselves are opened lazily, on the first message for their
 * database.
 */
void initialize_provsql_mmap(void);

/**
 * @brief Unmap and close the mmap files.
 *
 * Called by the background worker on shutdown to release resources and
 * ensure all dirty pages are synced to disk via @c msync().
 */
void destroy_provsql_mmap(void);

/**
 * @brief Main processing loop of the mmap background worker.
 *
 * Waits for gate-creation requests from backend processes, processes them
 * by writing to the mmap files, and handles SIGTERM for graceful shutdown.
 */
void provsql_mmap_main_loop(void);

/**
 * @brief How long the worker waits after a write before forcing the store
 *        to stable storage.
 *
 * The circuit store is outside PostgreSQL's WAL, so a committed
 * transaction's gates can still be sitting in the kernel's page cache
 * when the machine loses power.  Forcing them out this long after the
 * last write bounds the loss, the way @c synchronous_commit @c = @c off
 * bounds the heap's; @c provsql.synchronous_commit removes it entirely,
 * at the price of one flush per store-writing transaction.
 */
#define PROVSQL_STORE_FLUSH_INTERVAL_MS 200

/**
 * @brief What @c provsql.circuit_cleanup() reports.
 *
 * The three "before" figures are the store's size when the clean-up
 * started; the three "after" figures are the size of the rebuilt store
 * (or, on a dry run, only the live gate count, the rest being 0 -- the
 * wire and byte totals of a rewrite are not known without doing it).
 */
typedef struct provsql_cleanup_result {
  uint64 gates_before;  ///< Gate records before
  uint64 gates_after;   ///< Gate records kept
  uint64 wires_before;  ///< Child wires before
  uint64 wires_after;   ///< Child wires kept
  uint64 extra_before;  ///< Annotation bytes before
  uint64 extra_after;   ///< Annotation bytes kept
} provsql_cleanup_result;

/**
 * @brief Ask the worker to rebuild this database's store, keeping only
 *        what @p roots reach.
 *
 * The caller must hold the database exclusively; see
 * @c provsql.circuit_cleanup in @c circuit_cleanup.c.
 */
void provsql_circuit_cleanup_request(bool dry_run, const pg_uuid_t *roots,
                                     int64 nb_roots,
                                     provsql_cleanup_result *out);

/** @brief Force every open circuit to stable storage. */
void provsql_store_flush(void);

/**
 * @brief Note that this transaction has written to the circuit store.
 *
 * Arms the at-commit sync barrier (@c provsql.synchronous_commit) and the
 * @c PREPARE @c TRANSACTION refusal.
 */
void provsql_store_note_write(void);

/** @brief Whether this transaction has written to the circuit store. */
bool provsql_store_written(void);

/**
 * @brief Handle a single IPC message: read its payload and write its reply.
 *
 * The opcode @p c and the message header (@p db_oid and the database's
 * default tablespace @p db_tablespace) have already been consumed by the
 * caller.  Shared by the background-worker main loop (multi-process build)
 * and the synchronous in-process dispatcher.
 */
void provsql_mmap_dispatch(char c, Oid db_oid, Oid db_tablespace);

/**
 * @brief Create a gate from in-extension C/C++ code (cache + worker IPC).
 *
 * Internal entry point behind the SQL-callable @c create_gate(), without
 * Datum marshalling or gate-type-OID lookups; idempotent on
 * already-mapped tokens.
 *
 * @param token          UUID of the gate.
 * @param type           Gate type.
 * @param nb_children    Number of children.
 * @param children_data  Child UUIDs (may be NULL when @p nb_children is 0).
 */
void provsql_internal_create_gate(const pg_uuid_t *token, gate_type type,
                                  unsigned nb_children,
                                  const pg_uuid_t *children_data);

/**
 * @brief Outcome of a probability write, mirroring
 *        @c MMappedCircuit::SetProbResult across the IPC boundary.
 */
typedef enum provsql_set_prob_result {
  PROVSQL_SET_PROB_NOT_PROB_GATE = 0, ///< The gate carries no probability
  PROVSQL_SET_PROB_WRITTEN       = 1, ///< Written; undo on rollback
  PROVSQL_SET_PROB_UNCHANGED     = 2, ///< Already held exactly this value
  PROVSQL_SET_PROB_ALREADY_SET   = 3  ///< Holds a different value; refused
} provsql_set_prob_result;

/**
 * @brief Write a gate's probability from in-extension C/C++ code.
 *
 * Probabilities are written once (see @c MMappedCircuit::setProb), so
 * this reports which of the four cases applied rather than a bare
 * success flag.  Callers that write a probability on a gate they have
 * just created can treat anything but @c PROVSQL_SET_PROB_NOT_PROB_GATE
 * as success; @c set_prob() itself raises on
 * @c PROVSQL_SET_PROB_ALREADY_SET.
 *
 * Note that this is the raw store operation: it does not record the
 * write in the transaction's undo list.  SQL-level writers go through
 * @c provsql_set_prob_tracked() in @c probability_store.c so a rollback
 * drops what they wrote.
 *
 * @param token     UUID of the gate.
 * @param prob      Probability value in [0,1], or @c NaN to clear.
 * @param existing  On @c PROVSQL_SET_PROB_ALREADY_SET, the stored value.
 */
provsql_set_prob_result provsql_internal_set_prob(const pg_uuid_t *token,
                                                  double prob,
                                                  double *existing);

/**
 * @brief Drop a gate's probability, leaving it as it was before anyone
 *        wrote one.
 *
 * The rollback path of @c probability_store.c, and the only way to unset
 * a probability: there is none from SQL.  Unlike a write it does not arm
 * the at-commit sync barrier -- it runs when the transaction that would
 * have committed is already gone.
 */
void provsql_internal_clear_prob(const pg_uuid_t *token);

/**
 * @brief Report whether a probability has been written on a gate.
 *
 * Distinct from @c get_prob(), which reports the value an evaluation
 * would use and so answers 1 for a gate nobody gave a probability.
 *
 * @param token  UUID of the gate.
 * @param prob   On @c true return, the written probability.
 */
bool provsql_internal_get_prob_written(const pg_uuid_t *token, double *prob);

/**
 * @brief Fetch a gate's type and children, cache-first with a worker
 *        round-trip (and cache fill) on a miss.
 *
 * On return @p *children_out is a @c calloc'd array to be freed by the
 * caller, or @c NULL when the gate has no children.
 */
gate_type provsql_fetch_gate(const pg_uuid_t *token,
                             unsigned *nb_children_out,
                             pg_uuid_t **children_out);

/**
 * @brief Outcome of an annotation write, mirroring
 *        @c MMappedCircuit::SetAnnotationResult across the IPC boundary.
 */
typedef enum provsql_set_annotation_result {
  PROVSQL_SET_ANNOTATION_NO_SUCH_GATE = 0, ///< The token names no gate
  PROVSQL_SET_ANNOTATION_WRITTEN      = 1, ///< The gate had none and now has this one
  PROVSQL_SET_ANNOTATION_UNCHANGED    = 2, ///< The gate already held exactly this
  PROVSQL_SET_ANNOTATION_ALREADY_SET  = 3  ///< The gate holds a different one; refused
} provsql_set_annotation_result;

/**
 * @brief Write a gate's info fields from in-extension C/C++ code, once.
 *
 * Internal entry point behind the SQL-callable @c set_infos().  Like a
 * probability, an annotation is a fact appended to the gate; the two
 * fields are written once each, with @c 0 meaning "nothing recorded"
 * (see @c MMappedCircuit::setInfos).  Writing a different value over a
 * recorded one raises.
 *
 * @param token  UUID of the gate.
 * @param info1  First (gate-type-specific) info value, or @c 0 to leave it.
 * @param info2  Second info value, or @c 0 to leave it.
 */
void provsql_internal_set_infos(const pg_uuid_t *token, unsigned info1,
                                unsigned info2);

/**
 * @brief Write a gate's extra string from in-extension C/C++ code, once.
 *
 * Internal entry point behind the SQL-callable @c set_extra().  Writing
 * the string the gate already holds is a no-op -- which is also what
 * keeps the @c extra file from accumulating abandoned copies of it --
 * and writing a different one raises.
 *
 * @param token  UUID of the gate.
 * @param str    NUL-terminated extra string to attach.
 */
void provsql_internal_set_extra(const pg_uuid_t *token, const char *str);

#ifdef PROVSQL_INPROCESS_STORE

/**
 * @brief In-process replacement for a pipe write of a complete request.
 *
 * Appends the message in @p buf (@p len bytes) to the request FIFO and runs
 * @c provsql_mmap_dispatch once, leaving any reply in the response FIFO for
 * the caller's @c READB / @c READB_BYTES to consume.
 */
bool provsql_inproc_send(const char *buf, size_t len);

/** Growable shared write buffer used with @c STARTWRITEM / @c ADDWRITEM. */
extern char *buffer;
/** Current write position within @c buffer. */
extern unsigned bufferpos;
/** Allocated capacity of @c buffer. */
extern size_t buffercap;
/** @brief Ensure @c buffer can hold at least @p need bytes. */
void provsql_buffer_ensure(size_t need);

#define READM(var, type)   provsql_fifo_pop (&provsql_shared_state->req,  &(var), sizeof(type))
#define READB(var, type)   provsql_fifo_pop (&provsql_shared_state->resp, &(var), sizeof(type))
#define WRITEB(pvar, type) provsql_fifo_push(&provsql_shared_state->resp, (pvar), sizeof(type))
#define WRITEM(pvar, type) provsql_fifo_push(&provsql_shared_state->req,  (pvar), sizeof(type))

#define READB_BYTES(ptr, n) provsql_fifo_pop (&provsql_shared_state->resp, (ptr), (n))
#define READM_BYTES(ptr, n) provsql_fifo_pop (&provsql_shared_state->req,  (ptr), (n))
#define WRITEB_BYTES(ptr, n) provsql_fifo_push(&provsql_shared_state->resp, (ptr), (n))

#define STARTWRITEM() (bufferpos=0)
#define ADDWRITEM(pvar, type) (provsql_buffer_ensure(bufferpos+sizeof(type)), memcpy(buffer+bufferpos, pvar, sizeof(type)), bufferpos+=sizeof(type))
#define SENDWRITEM() provsql_inproc_send(buffer, bufferpos)

#else

/** @brief Read exactly @p n bytes from @p fd into @p dst; @c false on EOF/error. */
bool provsql_read_all(int fd, void *dst, size_t n);

/** Shared write buffer used with @c STARTWRITEM / @c ADDWRITEM / @c SENDWRITEM. */
extern char buffer[PIPE_BUF];
/** Current write position within @c buffer. */
extern unsigned bufferpos;

/** @brief Read one value of @p type from the background-to-main pipe. */
#define READM(var, type) (read(provsql_shared_state->pipebmr, &var, sizeof(type))==(ssize_t)sizeof(type)) // flawfinder: ignore
/** @brief Read one value of @p type from the main-to-background pipe. */
#define READB(var, type) (read(provsql_shared_state->pipembr, &var, sizeof(type))==(ssize_t)sizeof(type)) // flawfinder: ignore
/** @brief Write one value of @p type to the main-to-background pipe. */
#define WRITEB(pvar, type) (write(provsql_shared_state->pipembw, pvar, sizeof(type))!=-1)
/** @brief Write one value of @p type to the background-to-main pipe. */
#define WRITEM(pvar, type) (write(provsql_shared_state->pipebmw, pvar, sizeof(type))!=-1)

/** @brief Read exactly @p n bytes of a reply from the main-to-background pipe. */
#define READB_BYTES(ptr, n) provsql_read_all(provsql_shared_state->pipembr, (ptr), (n)) // flawfinder: ignore
/** @brief Read exactly @p n bytes of a request from the background-to-main pipe. */
#define READM_BYTES(ptr, n) provsql_read_all(provsql_shared_state->pipebmr, (ptr), (n)) // flawfinder: ignore
/** @brief Write @p n reply bytes to the main-to-background pipe. */
#define WRITEB_BYTES(ptr, n) (write(provsql_shared_state->pipembw, (ptr), (n))!=-1)

/** @brief Reset the shared write buffer for a new batched write. */
#define STARTWRITEM() (bufferpos=0)
/** @brief Append one value of @p type to the shared write buffer. */
#define ADDWRITEM(pvar, type) (memcpy(buffer+bufferpos, pvar, sizeof(type)), bufferpos+=sizeof(type))
/** @brief Flush the shared write buffer to the background-to-main pipe atomically. */
#define SENDWRITEM() (write(provsql_shared_state->pipebmw, buffer, bufferpos)!=-1)

#endif /* PROVSQL_INPROCESS_STORE */

/**
 * @brief Append the per-message database header to the write buffer.
 *
 * Every request carries the OID of the database it applies to and the
 * OID of that database's default tablespace, so the worker -- which runs
 * outside any transaction and cannot read @c pg_database -- can resolve
 * the directory holding the backing files.  Follows the opcode byte in
 * every message.
 */
#define ADDWRITEDB() (ADDWRITEM(&MyDatabaseId, Oid), ADDWRITEM(&MyDatabaseTableSpace, Oid))

#endif /* PROVSQL_COLUMN_NAME */
