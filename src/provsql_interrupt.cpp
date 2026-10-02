/**
 * @file provsql_interrupt.cpp
 * @brief The SIGINT handler that stops ProvSQL's evaluation loops.
 *
 * See @c provsql_interrupt.h for how an evaluation is cancelled.
 */
extern "C" {
#include "postgres.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "provsql_utils.h"
}

#include "provsql_interrupt.h"

#include <csignal>
#include <string>

#include <unistd.h>

#include "process_memory.h"

unsigned provsql_poll_count = 0;

/** Resident memory at the first evaluation of the statement, and when that
 *  statement started (the budget is per statement) */
static size_t baseline_rss = 0;
static TimestampTz baseline_statement = 0;

/** @brief The resident memory of this backend, in bytes, or 0 where the
 *  platform does not say. */
static size_t current_rss()
{
  return provsql_process_rss(getpid());
}

/**
 * @brief SIGINT handler that sets the global interrupted flag.
 *
 * The signal number argument is required by the @c signal() API but is
 * not used.
 *
 * In addition to the @c provsql_interrupted flag polled by the long
 * evaluation loops (Monte-Carlo, possible worlds, the semiring evaluation
 * of a circuit and its enumeration of worlds), we drive PG's
 * standard cancel pipeline (@c InterruptPending / @c QueryCancelPending
 * + @c SetLatch) the same way PG's own @c StatementCancelHandler does.
 * That makes a SIGINT delivered to the backend (e.g. via
 * @c pg_cancel_backend) outside of a @c system() wait turn into a
 * proper 57014 cancel at the next @c CHECK_FOR_INTERRUPTS instead of
 * being silently absorbed.  (The matching case where an external
 * compiler is running is handled in @c run_external_tool, which runs the
 * tool in its own process group and @c SIGKILLs that group on a pending
 * cancel, then lets @c CHECK_FOR_INTERRUPTS raise it.)
 */
void provsql_sigint_handler(int)
{
  provsql_interrupted = true;

  if (!proc_exit_inprogress) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
  SetLatch(MyLatch);
}

provsql_interrupt_scope::provsql_interrupt_scope()
{
  TimestampTz statement = GetCurrentStatementStartTimestamp();
  provsql_interrupted = false;
  prev_ = signal(SIGINT, provsql_sigint_handler);
  if (statement != baseline_statement) {
    baseline_statement = statement;
    baseline_rss = current_rss();
  }
}

provsql_interrupt_scope::~provsql_interrupt_scope()
{
  /* The flag stays: the catch that follows reads it */
  signal(SIGINT, prev_);
}

size_t provsql_memory_used(void)
{
  size_t rss;
  if (baseline_rss == 0 ||
      baseline_statement != GetCurrentStatementStartTimestamp())
    return 0;
  rss = current_rss();
  return rss > baseline_rss ? rss - baseline_rss : 0;
}

void provsql_check_memory(void)
{
  if (provsql_max_memory <= 0)
    return;
  if (provsql_memory_used() > (size_t)provsql_max_memory * 1024 * 1024)
    throw CircuitException(
      "the evaluation used more memory than provsql.max_memory (" +
      std::to_string(provsql_max_memory) + " MB)");
}

void provsql_cancel_if_interrupted(void)
{
  if (provsql_interrupted) {
    provsql_interrupted = false;
    CHECK_FOR_INTERRUPTS();
  }
}
