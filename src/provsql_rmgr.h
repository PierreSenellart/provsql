/**
 * @file provsql_rmgr.h
 * @brief WAL-logging the circuit store: the entry points other files use.
 *
 * See @c provsql_rmgr.c for what is logged, what replay does with it, and
 * why the feature is off by default.
 */
#ifndef PROVSQL_RMGR_H
#define PROVSQL_RMGR_H

#include "postgres.h"

/** Global variable set by the provsql.wal_logging run-time configuration
 *  parameter: when true, every mutation of the circuit store is written
 *  to the WAL before it is written to the store. */
extern bool provsql_wal_logging;

/**
 * @brief Register ProvSQL's resource manager with PostgreSQL.
 *
 * Must be called from @c _PG_init under @c shared_preload_libraries, and
 * is called unconditionally -- registration is what lets any WAL that
 * already contains ProvSQL records be replayed, whether or not this
 * cluster is currently writing them.  A no-op before PostgreSQL 15.
 */
void provsql_register_rmgr(void);

/**
 * @brief Write one store message to the WAL, if WAL logging is on.
 *
 * @param data  The complete message, starting with its opcode byte.
 * @param len   Its length in bytes.
 */
void provsql_wal_log_store_message(const char *data, size_t len);

/**
 * @brief Whether this process may write to the circuit store.
 *
 * False in a hot-standby backend: the standby's store is maintained by
 * replay, and a backend writing to it would make the two diverge for
 * good.  True in the startup process while it replays a ProvSQL record,
 * which is the one write recovery is supposed to make.
 */
bool provsql_store_write_allowed(void);

/**
 * @brief Feed a logged store message back to the worker.
 *
 * Implemented in @c provsql_mmap.c, where the IPC primitives live.
 */
void provsql_replay_store_message(const char *data, size_t len);

#endif /* PROVSQL_RMGR_H */
