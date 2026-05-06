/**
 * @file external_tool.h
 * @brief Helpers for invoking external command-line tools.
 *
 * @c run_external_tool() wraps @c system() so that the @c provsql.tool_search_path
 * GUC, if set, is prepended to @c $PATH for the duration of the call. The
 * server's pre-existing @c PATH is restored afterwards. Used by the d-DNNF
 * compilers (d4, c2d, minic2d, dsharp), the WeightMC model counter, and the
 * graph-easy DOT renderer.
 *
 * @c find_external_tool() walks the same locations and reports whether a
 * given binary is present and executable, so callers can fail with an
 * actionable error before composing a command line.
 *
 * @c format_external_tool_status() decodes a @c system() return value
 * into a human-readable message that distinguishes "tool not found"
 * (shell exit 127), "tool not executable" (126), "killed by signal", and
 * "ran and exited nonzero".
 *
 * In the standalone @c tdkc build (when @c TDKC is defined) the GUC layer is
 * unavailable; the helpers degenerate to a plain @c std::system() call and
 * a no-op @c find_external_tool.
 */
#ifndef PROVSQL_EXTERNAL_TOOL_H
#define PROVSQL_EXTERNAL_TOOL_H

#include <string>

#ifdef TDKC
#include <cstdlib>
inline int run_external_tool(const std::string &cmdline) {
  return std::system(cmdline.c_str());
}
inline std::string find_external_tool(const std::string &) {
  // No GUC layer in tdkc; assume present and let system() handle missing.
  return "/dev/null";
}
inline std::string format_external_tool_status(int, const std::string &tool) {
  return "Error executing " + tool;
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

/**
 * @brief Locate an external tool by name.
 *
 * Searches @c provsql_tool_search_path (colon-separated), then @c $PATH, for
 * an executable file matching @p name (via @c access(X_OK)). Returns the
 * full path on success, or the empty string if nothing matches.
 *
 * Names containing a slash are treated as paths and tested directly without
 * any directory walk.
 *
 * @param name  Bare executable name (e.g. @c "d4") or a path.
 * @return      Resolved path, or @c "" if @p name is not found.
 */
std::string find_external_tool(const std::string &name);

/**
 * @brief Decode a @c system() return value into a human-readable message.
 *
 * @param rv    Return value from @c system() (or @c run_external_tool()).
 * @param tool  Tool name, used as the message subject.
 * @return      Empty string when @p rv indicates success; otherwise a
 *              message that distinguishes "not found at runtime"
 *              (shell exit 127), "not executable" (126), "killed by
 *              signal N", "exited with status N", or @c system()
 *              itself failing.
 */
std::string format_external_tool_status(int rv, const std::string &tool);
#endif

#endif
