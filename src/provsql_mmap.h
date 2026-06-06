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
 * @brief Open (or create) the mmap files and initialise the circuit store.
 *
 * Called once by the background worker at startup.  Creates the four
 * mmap-backed data files if they do not yet exist and maps them into the
 * worker's address space.
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
 * @brief Handle a single IPC message: read its payload and write its reply.
 *
 * The opcode @p c and database OID @p db_oid have already been consumed by
 * the caller.  Shared by the background-worker main loop (multi-process
 * build) and the synchronous in-process dispatcher.
 */
void provsql_mmap_dispatch(char c, Oid db_oid);

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
 * @brief Set a gate's info fields from in-extension C/C++ code.
 *
 * Internal entry point behind the SQL-callable @c set_infos().
 *
 * @param token  UUID of the gate.
 * @param info1  First (gate-type-specific) info value.
 * @param info2  Second info value.
 */
void provsql_internal_set_infos(const pg_uuid_t *token, unsigned info1,
                                unsigned info2);

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

#endif /* PROVSQL_COLUMN_NAME */
