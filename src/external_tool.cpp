/**
 * @file external_tool.cpp
 * @brief Implementation of @c run_external_tool().
 *
 * Reads the @c provsql.tool_search_path GUC (exposed as
 * @c provsql_tool_search_path) and, if non-empty, prepends it to @c $PATH
 * before calling @c system(), restoring the prior @c PATH afterwards.
 */
extern "C" {
#include "postgres.h"
#include "provsql_utils.h"

#include <stdlib.h>
}

#include "external_tool.h"

#include <string>

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
    if (had_path) {
      new_path += ':';
      new_path += saved_path;
    }
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
