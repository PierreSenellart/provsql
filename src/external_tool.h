/**
 * @file external_tool.h
 * @brief Helper for invoking external command-line tools.
 *
 * @c run_external_tool() wraps @c system() so that the @c provsql.tool_search_path
 * GUC, if set, is prepended to @c $PATH for the duration of the call. The
 * server's pre-existing @c PATH is restored afterwards. Used by the d-DNNF
 * compilers (d4, c2d, minic2d, dsharp), the WeightMC model counter, and the
 * graph-easy DOT renderer.
 *
 * In the standalone @c tdkc build (when @c TDKC is defined) the GUC layer is
 * unavailable; the helper degenerates to a plain @c std::system() call.
 */
#ifndef PROVSQL_EXTERNAL_TOOL_H
#define PROVSQL_EXTERNAL_TOOL_H

#include <string>

#ifdef TDKC
#include <cstdlib>
inline int run_external_tool(const std::string &cmdline) {
  return std::system(cmdline.c_str());
}
#else
/**
 * @brief Run a shell command line, optionally extending @c PATH.
 *
 * If @c provsql_tool_search_path is non-empty, it is prepended (with @c :)
 * to @c $PATH before @c system() is called and restored afterwards.
 *
 * @param cmdline  Shell command line, passed verbatim to @c /bin/sh -c.
 * @return         The raw return value of @c system().
 */
int run_external_tool(const std::string &cmdline);
#endif

#endif
