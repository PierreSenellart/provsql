/**
 * @file provenance.c
 * @brief SQL function @c provenance() – the certain provenance @c gate_one().
 *
 * In a query over provenance-tracked data the planner hook replaces every
 * call to @c provsql.provenance() by the provenance of the row, so this
 * function only runs where nothing gives the row a provenance: a query
 * over untracked relations, @c VALUES, or no relation at all, or one
 * planned with @c provsql.active off.  Such a row is in every possible
 * world, and its provenance is the semiring one, which is what an
 * untracked row already gets inside a tracked query.
 */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance);

/**
 * @brief @c provsql.provenance() where the planner hook left it in place.
 *
 * @return The UUID of @c gate_one(), the semiring one.
 */
Datum provenance(PG_FUNCTION_ARGS)
{
  const constants_t constants = get_constants(true);
  return OidFunctionCall0(constants.OID_FUNCTION_GATE_ONE);
}
