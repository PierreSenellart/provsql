/**
 * @file provsql_interrupt.h
 * @brief Cancellation of the long C++ evaluation loops.
 *
 * A query cancel or a @c statement_timeout reaches the backend as a SIGINT.
 * While an evaluation runs, the entry point installs
 * @c provsql_sigint_handler, which sets @c provsql_interrupted (and PG's own
 * cancel flags); the loops poll it with @c provsql_poll_interrupt and throw,
 * unwinding the C++ stack (and freeing what it holds) instead of a
 * @c longjmp through it.  The entry point restores the previous handler,
 * then lets @c CHECK_FOR_INTERRUPTS raise PG's native cancel (57014).
 *
 * An entry point does so as
 * @code
 *   try {
 *     provsql_interrupt_scope interrupt_scope;
 *     ...
 *   } catch (const std::exception &e) {
 *     provsql_cancel_if_interrupted();
 *     provsql_error("...: %s", e.what());
 *   }
 * @endcode
 * the scope restoring the handler as the @c try is left.
 *
 * The same polls enforce @c provsql.max_memory: every few thousand of them,
 * the backend's resident memory is compared with what it was at the first
 * evaluation of the statement, and past the budget the evaluation stops with
 * an error, as it does on a cancel.
 * The handler is defined in @c provsql_interrupt.cpp.
 */
#ifndef PROVSQL_INTERRUPT_H
#define PROVSQL_INTERRUPT_H

#include "Circuit.h"

extern "C" {
/** Set by @c provsql_sigint_handler: this backend received a cancel. */
extern bool provsql_interrupted;

/**
 * @brief SIGINT handler setting @c provsql_interrupted, and PG's cancel
 *        flags as PG's own @c StatementCancelHandler does.
 */
void provsql_sigint_handler(int);

/**
 * @brief Raise PG's cancel if the evaluation stopped for one; nothing
 *        otherwise.  Called in a @c catch, once the scope is gone.
 */
void provsql_cancel_if_interrupted(void);

/** @c provsql.max_memory, in MB (0: no limit). */
extern int provsql_max_memory;

/** Polls since the last check of the memory budget. */
extern unsigned provsql_poll_count;

/**
 * @brief Throw if the evaluations of the statement have made the backend's
 *        resident memory grow by more than @c provsql.max_memory.
 */
void provsql_check_memory(void);

/**
 * @brief How much the backend's resident memory has grown since the first
 *        evaluation of the statement, in bytes (0 before it).  An external
 *        tool's memory is counted against the same budget, added to this.
 */
size_t provsql_memory_used(void);

/**
 * @brief Stop the evaluation at a resource limit (@c provsql.max_memory,
 *        @c provsql.max_worlds): throws, like a cancel, after recording the
 *        limit, which @c provsql_cancel_if_interrupted then raises as a
 *        @c program_limit_exceeded error (54000) with @p tag on its DETAIL
 *        line and @p hint as its HINT, instead of an internal error.
 */
void provsql_limit_exceeded(const char *tag, const char *message,
                            const char *hint);
}

/**
 * @brief The SIGINT handler installed for the lifetime of the object, the
 *        previous one restored as it ends (a C++ exception included).
 */
class provsql_interrupt_scope
{
  void (*prev_)(int); ///< The handler to restore

public:
  provsql_interrupt_scope();
  ~provsql_interrupt_scope();
  provsql_interrupt_scope(const provsql_interrupt_scope &) = delete;
  provsql_interrupt_scope &operator=(const provsql_interrupt_scope &) = delete;
};


/** @brief Throw where an evaluation is to stop, its query cancelled. */
inline void provsql_poll_interrupt()
{
  if (provsql_interrupted)
    throw CircuitException("Interrupted");
  if (provsql_max_memory > 0 && (++provsql_poll_count & 4095) == 0)
    provsql_check_memory();
}

#endif /* PROVSQL_INTERRUPT_H */
