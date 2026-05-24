/**
 * @file external_tool.cpp
 * @brief Implementation of the external-tool helpers.
 *
 * Reads the @c provsql.tool_search_path GUC (exposed as
 * @c provsql_tool_search_path) and uses it both to extend @c $PATH around
 * @c system() and to drive the pre-flight @c find_external_tool() lookup.
 */
extern "C" {
#include "postgres.h"
#include "provsql_utils.h"
#include "miscadmin.h"

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
}

#include "external_tool.h"

#include <string>

// PATH that /bin/sh resolves binaries against when the environment has no
// PATH set. PostgreSQL backends inherit no PATH from systemd, so
// getenv("PATH") is NULL inside the server; without an explicit fallback,
// setting PATH to "<GUC>" alone would mask /usr/local/bin and friends
// (dash's compiled-in default), making the GUC-set case strictly narrower
// than the GUC-empty case. Matches dash's _PATH_STDPATH on Debian/Ubuntu
// and bash's default on macOS.
static const char *DEFAULT_PATH =
    "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

/*
 * Run @p cmdline via @c /bin/sh @c -c in its OWN process group, polling for
 * a query cancel / backend termination while it runs.  Returns a wait(2)
 * status (so @c WIFEXITED / @c WIFSIGNALED / @c WEXITSTATUS decode exactly
 * as the old @c system() return did), or -1 if @c fork failed.
 *
 * Why not @c system(): it runs the child in the BACKEND's process group,
 * and statement_timeout / pg_cancel_backend deliver SIGINT to that group
 * (PostgreSQL backends @c setsid, and @c StatementTimeoutHandler does
 * @c kill(-MyProcPid, SIGINT)).  A well-behaved tool dies on it, but a tool
 * that catches/ignores SIGINT -- or that forks a worker into another
 * process group, as KCBox/Panini does -- survives, so the timeout is
 * silently not honoured (a single long OBDD compile runs unbounded).  Here
 * the child leads its own process group, and when a cancel/terminate is
 * pending we @c SIGKILL that whole group: uncatchable, and it reaches any
 * forked workers.  The pending interrupt is then raised by the
 * @c CHECK_FOR_INTERRUPTS() in @c run_external_tool, with the child reaped.
 */
static int run_in_own_pgroup(const std::string &cmdline)
{
  fflush(NULL);                       /* flush stdio before fork, like system() */

  pid_t child = fork();
  if (child < 0)
    return -1;                        /* mirror system()'s failure return */

  if (child == 0) {
    /* Child: lead a new process group so a later killpg targets this whole
     * subtree (including tools that fork their own workers) and never the
     * backend.  exec resets caught signals to default, so SIGINT / SIGTERM
     * / SIGKILL terminate the tool normally. */
    setpgid(0, 0);
    execl("/bin/sh", "sh", "-c", cmdline.c_str(), (char *) NULL);
    _exit(127);                       /* exec failed: shell-style "not found" */
  }

  /* Parent: close the setpgid race (whichever process runs first wins). */
  setpgid(child, child);

  int status = 0;
  for (;;) {
    pid_t w = waitpid(child, &status, WNOHANG);
    if (w == child)
      break;                          /* tool finished; status is set */
    if (w < 0 && errno != EINTR) {
      status = -1;                    /* unexpected; surface as failure */
      break;
    }
    /* A pending query cancel (statement_timeout, pg_cancel_backend) or
     * backend termination (pg_terminate_backend / SIGTERM) must stop the
     * tool now.  Kill its whole process group hard and reap it.  We do NOT
     * call CHECK_FOR_INTERRUPTS() while the child is alive: a throw there
     * would leak the running child as an orphan -- the very bug being
     * fixed.  The caller's CHECK_FOR_INTERRUPTS() (below) raises the
     * pending interrupt once the child is reaped. */
    if (QueryCancelPending || ProcDiePending) {
      killpg(child, SIGKILL);
      pid_t r;
      do { r = waitpid(child, &status, 0); } while (r < 0 && errno == EINTR);
      break;
    }
    pg_usleep(10000);                 /* 10 ms; an arriving signal wakes us via EINTR */
  }
  return status;
}

int run_external_tool(const std::string &cmdline) {
  bool override_path = (provsql_tool_search_path != NULL
                        && provsql_tool_search_path[0] != '\0');
  std::string saved_path;
  bool had_path = false;

  if (override_path) {
    const char *cur = getenv("PATH");
    if (cur != NULL) {
      saved_path = cur;
      had_path = true;
    }
    std::string new_path(provsql_tool_search_path);
    new_path += ':';
    new_path += had_path ? saved_path : DEFAULT_PATH;
    setenv("PATH", new_path.c_str(), 1);
  }

  int rv = run_in_own_pgroup(cmdline);

  if (override_path) {
    if (had_path)
      setenv("PATH", saved_path.c_str(), 1);
    else
      unsetenv("PATH");
  }

  /* If a cancel/terminate fired while the tool ran, run_in_own_pgroup has
   * already killed and reaped it; raise the interrupt now (cleanly, with no
   * child left running) so it surfaces as query-cancelled rather than being
   * masked by a downstream "tool killed by signal" error.  A no-op when no
   * interrupt is pending. */
  CHECK_FOR_INTERRUPTS();

  return rv;
}

std::string find_external_tool(const std::string &name) {
  // Path-like names (containing '/') are tested directly without any
  // search-path walk; this matches POSIX execvp semantics.
  if (name.find('/') != std::string::npos)
    return access(name.c_str(), X_OK) == 0 ? name : "";

  // Delegate the search to /bin/sh via `command -v`, routed through
  // run_external_tool() so the GUC override is honoured. This reuses
  // exactly the PATH resolution that the eventual tool invocation will
  // see, including the shell's compiled-in default when the environment
  // has no PATH (typical inside a PostgreSQL backend).
  //
  // Single-quoting `name` defends against shell metacharacters; the
  // five tool names provsql actually uses ("d4", "c2d", "minic2d",
  // "dsharp", "weightmc", "graph-easy") contain none.
  std::string check = "command -v '" + name + "' >/dev/null 2>&1";
  // run_external_tool runs the probe in its own process group and raises
  // any pending statement_timeout / cancel itself (so it does not surface
  // as a spurious "tool not found").
  int rv = run_external_tool(check);

  return rv == 0 ? name : "";
}

std::string format_external_tool_status(int rv, const std::string &tool) {
  if (rv == 0)
    return "";
  if (rv == -1)
    return tool + " could not be invoked (system() returned -1)";
  if (WIFSIGNALED(rv))
    return tool + " terminated by signal "
           + std::to_string(WTERMSIG(rv));
  if (WIFEXITED(rv)) {
    int code = WEXITSTATUS(rv);
    if (code == 127)
      return tool + " was not found at runtime (shell exit 127); "
             "install it or add its directory to provsql.tool_search_path";
    if (code == 126)
      return tool + " is not executable (shell exit 126); "
             "check permissions on the binary";
    return tool + " exited with status " + std::to_string(code);
  }
  return tool + " failed with raw status " + std::to_string(rv);
}
