extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(where_provenance);
}

#include <csignal>

#include "BooleanCircuit.h"

using namespace std;

/* copied with small changes from uuid.c */

static Datum where_provenance_internal
  (Datum token)
{
  constants_t constants;
  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  Datum arguments[1]={token};
  Oid argtypes[1]={constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};
  
  SPI_connect();

  if(SPI_execute_with_args(
      "SELECT * FROM provsql.sub_circuit_for_where($1)", 2, argtypes, arguments, nulls, true, 0)
      == SPI_OK_SELECT) {
    // TODO
  }

  SPI_finish();

  // TODO

  return (Datum) 0;
}

Datum where_provenance(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);

    return where_provenance_internal(token);
  } catch(const std::exception &e) {
    elog(ERROR, "probability_evaluate: %s", e.what());
  } catch(...) {
    elog(ERROR, "probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}
