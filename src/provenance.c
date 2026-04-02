#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance);

Datum provenance(PG_FUNCTION_ARGS)
{
  provsql_error("provenance() called on a table without provenance");
}
