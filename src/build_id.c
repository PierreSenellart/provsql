/**
 * @file build_id.c
 * @brief The build of the ProvSQL library: @c provsql.build_id().
 *
 * The extension's version (@c 1.13.0-dev, say) is the same for every build
 * of a development cycle.  What tells two builds apart is the commit they
 * were built from, which the Makefile writes into @c build_id.h: the output
 * of @c "git describe --tags --always --dirty", or @c unknown outside a git
 * checkout (a source archive).
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "build_id.h"

PG_FUNCTION_INFO_V1(build_id);
/** @brief SQL entry point: the build of the loaded library, as text. */
Datum build_id(PG_FUNCTION_ARGS)
{
  PG_RETURN_TEXT_P(cstring_to_text(PROVSQL_BUILD_ID));
}
