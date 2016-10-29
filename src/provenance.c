#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance);

Datum provenance(PG_FUNCTION_ARGS)
{
  if(!provsql_shared_library_loaded) {
    elog(ERROR, "provsql shared library not loaded");
  } else {
    elog(ERROR, "provenance() called on a table without provenance");
  }
}
