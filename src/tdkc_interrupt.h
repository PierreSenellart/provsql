/**
 * @file tdkc_interrupt.h
 * @brief Build-loop interrupt hook for the standalone @c tdkc binary.
 *
 * In the @c tdkc build, @c CHECK_FOR_INTERRUPTS() (called from the hot loops
 * of the tree-decomposition and d-DNNF construction) resolves to
 * @c provsql_tdkc_poll() instead of a no-op.  When @c tdkc runs as a KCMCP
 * server, the poll services the active connection mid-job: it answers
 * @c PING with @c PONG, emits rate-limited @c PROGRESS frames, and throws to
 * abort the build on @c CANCEL or a @c timeout_ms deadline.  When no session
 * is active (the plain command-line mode), it is a no-op.
 *
 * This header is only meaningful in the @c tdkc build; the PostgreSQL
 * extension uses the real @c CHECK_FOR_INTERRUPTS.
 */
#ifndef PROVSQL_TDKC_INTERRUPT_H
#define PROVSQL_TDKC_INTERRUPT_H

void provsql_tdkc_poll();

#endif
