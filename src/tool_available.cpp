/**
 * @file tool_available.cpp
 * @brief SQL function @c provsql.tool_available() – report whether an
 *        external tool is on the backend's resolved PATH.
 *
 * The check uses the same @c find_external_tool() helper that the
 * compilers / WMC counters / GraphViz wrappers themselves consult, so
 * the result reflects exactly what a subsequent
 * @c probability_evaluate('compilation', '\<tool\>') call would see,
 * including the @c provsql.tool_search_path GUC prepended to @c $PATH.
 *
 * Used by Studio (and any other client) to filter the list of usable
 * knowledge compilers / model counters before offering them in the UI
 * or running the probability benchmark.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

PG_FUNCTION_INFO_V1(tool_available);
}

#include "external_tool.h"
#include "provsql_utils_cpp.h"

#include <string>

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Argument: @c name (text), a bare executable name (e.g. @c "d4") or
 * an absolute path. Names with a slash bypass the PATH walk and are
 * tested directly with @c access(X_OK).
 *
 * Returns: @c true iff @c find_external_tool returns a non-empty
 * resolved path. NULL input returns NULL (STRICT in the SQL
 * declaration). A trailing whitespace-only input returns false.
 */
Datum tool_available(PG_FUNCTION_ARGS)
{
  try {
    text *name_text = PG_GETARG_TEXT_PP(0);
    std::string name(VARDATA_ANY(name_text), VARSIZE_ANY_EXHDR(name_text));
    if (name.empty())
      PG_RETURN_BOOL(false);
    PG_RETURN_BOOL(!find_external_tool(name).empty());
  } catch (const std::exception &e) {
    provsql_error("%s", e.what());
  } catch (...) {
    provsql_error("Unknown exception");
  }
  PG_RETURN_BOOL(false);
}
