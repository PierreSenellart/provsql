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

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#endif

unsigned provsql_poll_count = 0;

/** Resident memory at the first evaluation of the statement, and when that
 *  statement started (the budget is per statement) */
static size_t baseline_rss = 0;
static TimestampTz baseline_statement = 0;

/**
 * @brief The resident memory of this backend, in bytes, or 0 where the
 *        platform does not say.
 */
static size_t current_rss()
{
#if defined(__linux__)
  char buf[128];
  int fd = open("/proc/self/statm", O_RDONLY);
  ssize_t len;
  unsigned long size, resident;
  if (fd < 0)
    return 0;
  len = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (len <= 0)
    return 0;
  buf[len] = '\0';
  if (sscanf(buf, "%lu %lu", &size, &resident) != 2)
    return 0;
  return (size_t)resident * (size_t)sysconf(_SC_PAGESIZE);
#elif defined(__APPLE__)
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                (task_info_t)&info, &count) != KERN_SUCCESS)
    return 0;
  return (size_t)info.resident_size;
#elif defined(__FreeBSD__)
  struct kinfo_proc kp;
  size_t len = sizeof(kp);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
  if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
    return 0;
  return (size_t)kp.ki_rssize * (size_t)getpagesize();
#else
  return 0;
#endif
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

void provsql_check_memory(void)
{
  size_t rss, budget;
  if (provsql_max_memory <= 0 || baseline_rss == 0 ||
      baseline_statement != GetCurrentStatementStartTimestamp())
    return;
  rss = current_rss();
  budget = (size_t)provsql_max_memory * 1024 * 1024;
  if (rss > baseline_rss && rss - baseline_rss > budget)
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
