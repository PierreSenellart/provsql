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
}

#endif /* PROVSQL_INTERRUPT_H */
