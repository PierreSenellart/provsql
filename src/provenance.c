/**
 * @file provenance.c
 * @brief SQL function @c provenance() – error stub for untracked tables.
 *
 * The SQL function @c provsql.provenance() is added to every table that
 * has provenance tracking enabled.  When a query references the
 * @c provsql column of such a table directly (without going through the
 * planner hook), this stub is called and immediately raises an error.
 *
 * In normal operation the planner hook rewrites queries that reference
 * provenance-tracked tables before this function can be called, so the
 * error message is only seen if @c provenance() is invoked on a relation
 * that does not have a @c provsql column.
 */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance);

/**
 * @brief Error stub for @c provsql.provenance() on untracked tables.
 *
 * Always raises a @c provsql_error and never returns.
 * @return Never returns (always raises an error).
 */
Datum provenance(PG_FUNCTION_ARGS)
{
  provsql_error("provenance() called on a table without provenance");
}
