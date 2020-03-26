#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "access/htup_details.h"

#include "provsql_utils.h"
#include "agg_token.h"

PG_FUNCTION_INFO_V1(agg_token_in);

Datum
agg_token_in(PG_FUNCTION_ARGS) 
{
    char        *str = PG_GETARG_CSTRING(0);
    PG_RETURN_CSTRING(str);
}

PG_FUNCTION_INFO_V1(agg_token_out);

Datum
agg_token_out(PG_FUNCTION_ARGS)
{
  Datum token = PG_GETARG_DATUM(0);

  constants_t constants;
  char *result = NULL;
  char nulls[1]={' '};

  Datum arguments[1]={token};
  Oid argtypes[1];

  if(PG_ARGISNULL(0))
    PG_RETURN_CSTRING("");

  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  argtypes[0]=constants.OID_TYPE_AGG_TOKEN;

  SPI_connect();

  if(SPI_execute_with_args(
        "SELECT val FROM provsql.aggregation_circuit_extra WHERE gate::uuid=$1::uuid",
        1,
        argtypes,
        arguments,
        nulls,
        true,
        1) != SPI_OK_SELECT || SPI_processed != 1) {
    elog(WARNING, "Cannot get aggregation value from aggregation_circuit_extra");
  } else {
    result = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
  }

  SPI_finish();

  if(result == NULL)
    PG_RETURN_CSTRING("");
  else
    PG_RETURN_CSTRING(result);
}
