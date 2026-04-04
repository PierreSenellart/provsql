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

#include "postgres.h"
#include "provsql_utils.h"

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

/** Shared write buffer used with @c STARTWRITEM / @c ADDWRITEM / @c SENDWRITEM. */
extern char buffer[PIPE_BUF];
/** Current write position within @c buffer. */
extern unsigned bufferpos;

/** @brief Read one value of @p type from the background-to-main pipe. */
#define READM(var, type) (read(provsql_shared_state->pipebmr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
/** @brief Read one value of @p type from the main-to-background pipe. */
#define READB(var, type) (read(provsql_shared_state->pipembr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
/** @brief Write one value of @p type to the main-to-background pipe. */
#define WRITEB(pvar, type) (write(provsql_shared_state->pipembw, pvar, sizeof(type))!=-1)
/** @brief Write one value of @p type to the background-to-main pipe. */
#define WRITEM(pvar, type) (write(provsql_shared_state->pipebmw, pvar, sizeof(type))!=-1)

/** @brief Reset the shared write buffer for a new batched write. */
#define STARTWRITEM() (bufferpos=0)
/** @brief Append one value of @p type to the shared write buffer. */
#define ADDWRITEM(pvar, type) (memcpy(buffer+bufferpos, pvar, sizeof(type)), bufferpos+=sizeof(type))
/** @brief Flush the shared write buffer to the background-to-main pipe atomically. */
#define SENDWRITEM() (write(provsql_shared_state->pipebmw, buffer, bufferpos)!=-1)

#endif /* PROVSQL_COLUMN_NAME */
