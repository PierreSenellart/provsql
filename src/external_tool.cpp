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

#include <stdlib.h>
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

  int rv = system(cmdline.c_str());

  if (override_path) {
    if (had_path)
      setenv("PATH", saved_path.c_str(), 1);
    else
      unsetenv("PATH");
  }

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
  return run_external_tool(check) == 0 ? name : "";
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
