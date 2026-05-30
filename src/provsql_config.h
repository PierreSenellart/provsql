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

/*
 * PROVSQL_NO_SUBPROCESS: no subprocesses (fork/exec) and no sockets are
 * available in the WASM sandbox.  Under it, the external knowledge-compiler
 * CLIs, the KCMCP socket client, and the KCMCP supervisor worker are
 * compiled out; probability falls back to the in-process tree-decomposition
 * compiler and Monte Carlo.  Tied to the platform (__EMSCRIPTEN__), not to
 * PROVSQL_INPROCESS_STORE, so a native build -- even one forcing the
 * in-process store for testing -- keeps the subprocess/socket paths and
 * stays a faithful regression baseline; the guarded branches are exercised
 * by the actual WASM build.
 */
#ifdef __EMSCRIPTEN__
#define PROVSQL_NO_SUBPROCESS 1
#endif

#endif /* PROVSQL_CONFIG_H */
