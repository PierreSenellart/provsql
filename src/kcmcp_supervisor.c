/**
 * @file kcmcp_supervisor.c
 * @brief Background worker that launches and supervises the managed KCMCP
 *        knowledge-compiler server (the "managed mode" of the KCMCP client).
 *
 * When the @c provsql.kcmcp_server GUC is non-empty it is a shell command to
 * start a KCMCP server (see @c doc/source/dev/kc-server-protocol.rst), with
 * the literal @c {endpoint} replaced by a Unix-socket path this worker picks.
 * The worker forks/execs that command in its own process group, publishes the
 * endpoint in shared memory (read by the in-extension client,
 * @c kcmcp_client.cpp, for a registry record whose @c endpoint is
 * @c 'managed'), and supervises it: on the server's exit it relaunches, on a
 * config reload it restarts a changed command, and on shutdown it kills the
 * whole group.  When the GUC is empty the worker simply idles on its latch.
 *
 * Modeled on @c RegisterProvSQLMMapWorker (src/provsql_mmap.c); like it, the
 * worker needs only @c BGWORKER_SHMEM_ACCESS (no database connection): the GUC
 * is a process-global and the endpoint lives in the shared segment.
 */
#include "postgres.h"
#include "miscadmin.h"
#include "pgstat.h"                  /* PG_WAIT_EXTENSION */
#include "lib/stringinfo.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"   /* PostPortNumber */
#include "storage/latch.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#include "provsql_utils.h"
#include "provsql_shmem.h"
#include "provsql_error.h"

#if PG_VERSION_NUM < 120000
/* WL_EXIT_ON_PM_DEATH (have WaitLatch exit on postmaster death) was introduced
 * in PostgreSQL 12; on PG 10/11 fall back to the older WL_POSTMASTER_DEATH and
 * leave the supervise loop ourselves when WaitLatch reports it (see kcmcp_wait). */
#define WL_EXIT_ON_PM_DEATH WL_POSTMASTER_DEATH
#endif

static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup = false;

static void kcmcp_sigterm(SIGNAL_ARGS)
{
  int save_errno = errno;
  got_sigterm = true;
  SetLatch(MyLatch);
  errno = save_errno;
}

static void kcmcp_sighup(SIGNAL_ARGS)
{
  int save_errno = errno;
  got_sighup = true;
  SetLatch(MyLatch);
  errno = save_errno;
}

const char *provsql_kcmcp_managed_endpoint(void)
{
  static char buf[256];
  buf[0] = '\0';
  if (provsql_shared_state == NULL)
    return buf;
  provsql_shmem_lock_shared();
  strlcpy(buf, provsql_shared_state->kcmcp_endpoint, sizeof(buf));
  provsql_shmem_unlock();
  return buf;
}

static void publish_endpoint(const char *endpoint)
{
  if (provsql_shared_state == NULL)
    return;
  provsql_shmem_lock_exclusive();
  strlcpy(provsql_shared_state->kcmcp_endpoint, endpoint,
          sizeof(provsql_shared_state->kcmcp_endpoint));
  provsql_shmem_unlock();
}

/* Build the server command by replacing the first "{endpoint}" in the GUC
 * template with @p endpoint.  Returns a palloc'd string, or NULL if the
 * template lacks the placeholder. */
static char *build_server_command(const char *tmpl, const char *endpoint)
{
  const char *p = strstr(tmpl, "{endpoint}");
  StringInfoData s;
  if (p == NULL)
    return NULL;
  initStringInfo(&s);
  appendBinaryStringInfo(&s, tmpl, p - tmpl);
  appendStringInfoString(&s, endpoint);
  appendStringInfoString(&s, p + strlen("{endpoint}"));
  return s.data;
}

/* Fork/exec the server command in its own process group; returns the child
 * pid, or -1 on failure. */
static pid_t launch_server(const char *cmd)
{
  pid_t child;
  fflush(NULL);
  child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    setpgid(0, 0);
    execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
    _exit(127);
  }
  setpgid(child, child);
  return child;
}

static void kill_server(pid_t child)
{
  int status;
  pid_t r;
  if (child <= 0)
    return;
  killpg(child, SIGKILL);
  do { r = waitpid(child, &status, 0); } while (r < 0 && errno == EINTR);
}

/* Wait on the latch (and an optional timeout), resetting it.  Returns true if
 * the postmaster died: on PG >= 12 WL_EXIT_ON_PM_DEATH makes WaitLatch exit the
 * process itself (so this never returns true); on PG 10/11 it degrades to
 * WL_POSTMASTER_DEATH and WaitLatch returns with that bit set, which the caller
 * turns into a clean exit from the supervise loop. */
static bool kcmcp_wait(long timeout_ms)
{
  int events = WL_LATCH_SET | WL_EXIT_ON_PM_DEATH;
  int rc;
  if (timeout_ms >= 0)
    events |= WL_TIMEOUT;
  rc = WaitLatch(MyLatch, events, timeout_ms, PG_WAIT_EXTENSION);
  ResetLatch(MyLatch);
  return (rc & WL_POSTMASTER_DEATH) != 0;
}

PGDLLEXPORT void provsql_kcmcp_worker(Datum ignored);

void provsql_kcmcp_worker(Datum ignored)
{
  pid_t child = -1;
  char endpoint[256];

  (void) ignored;
  pqsignal(SIGTERM, kcmcp_sigterm);
  pqsignal(SIGHUP, kcmcp_sighup);
  BackgroundWorkerUnblockSignals();

  snprintf(endpoint, sizeof(endpoint), "unix:/tmp/.provsql-kcmcp-%d.sock",
           PostPortNumber);

  for (;;) {
    bool configured = (provsql_kcmcp_server != NULL
                       && provsql_kcmcp_server[0] != '\0');

    if (got_sigterm)
      break;

    if (got_sighup) {
      got_sighup = false;
      ProcessConfigFile(PGC_SIGHUP);
      /* A changed command (or one just cleared) takes effect by recycling the
       * child; the relaunch below picks up the new template. */
      if (child > 0) {
        kill_server(child);
        child = -1;
        publish_endpoint("");
      }
      continue;
    }

    if (!configured) {
      if (child > 0) {
        kill_server(child);
        child = -1;
        publish_endpoint("");
      }
      if (kcmcp_wait(-1))
        break;
      continue;
    }

    if (child <= 0) {
      char *cmd = build_server_command(provsql_kcmcp_server, endpoint);
      if (cmd == NULL) {
        provsql_warning("provsql.kcmcp_server has no {endpoint} placeholder; "
                        "not launching the managed KCMCP server");
        if (kcmcp_wait(-1))
          break;
        continue;
      }
      child = launch_server(cmd);
      pfree(cmd);
      if (child < 0) {
        provsql_warning("could not fork the managed KCMCP server");
      } else {
        publish_endpoint(endpoint);
        provsql_log("managed KCMCP server started (pid %d) on %s",
                    (int) child, endpoint);
      }
    }

    /* Supervise: wake on the child's exit (SIGCHLD interrupts the wait), a
     * signal, or the 1 s timeout, then reap non-blockingly. */
    if (kcmcp_wait(1000))
      break;

    if (child > 0) {
      int status;
      pid_t w = waitpid(child, &status, WNOHANG);
      if (w == child) {
        publish_endpoint("");
        provsql_log("managed KCMCP server (pid %d) exited; relaunching",
                    (int) child);
        child = -1;   /* the next iteration relaunches */
      }
    }
  }

  if (child > 0)
    kill_server(child);
  publish_endpoint("");
}

void RegisterProvSQLKCMCPWorker(void)
{
  BackgroundWorker worker;
  memset(&worker, 0, sizeof(worker));
  snprintf(worker.bgw_name, BGW_MAXLEN, "ProvSQL KCMCP Supervisor");
#if PG_VERSION_NUM >= 110000
  snprintf(worker.bgw_type, BGW_MAXLEN, "ProvSQL KCMCP");
#endif
  worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
  worker.bgw_start_time = BgWorkerStart_PostmasterStart;
  worker.bgw_restart_time = 1;
  snprintf(worker.bgw_library_name, BGW_MAXLEN, "provsql");
  snprintf(worker.bgw_function_name, BGW_MAXLEN, "provsql_kcmcp_worker");
  worker.bgw_main_arg = (Datum) 0;
  worker.bgw_notify_pid = 0;
  RegisterBackgroundWorker(&worker);
}
