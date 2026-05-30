/**
 * @file provsql_config.h
 * @brief Build-configuration switches shared across the C and C++ sources.
 *
 * @c PROVSQL_INPROCESS_STORE selects the single-process circuit store: the
 * background worker, the shared-memory segment, the LWLock, and the
 * inter-process pipes are replaced by an in-memory request/response FIFO
 * and a synchronous in-process dispatch.  It is the configuration used for
 * the browser/WASM target (where there is exactly one PostgreSQL process
 * and no background workers), and can also be forced on a native build for
 * testing the in-process path:
 *
 *     make CPPFLAGS=-DPROVSQL_INPROCESS_STORE
 */
#ifndef PROVSQL_CONFIG_H
#define PROVSQL_CONFIG_H

#if defined(__EMSCRIPTEN__) && !defined(PROVSQL_MULTIPROCESS)
#define PROVSQL_INPROCESS_STORE 1
#endif

#endif /* PROVSQL_CONFIG_H */
