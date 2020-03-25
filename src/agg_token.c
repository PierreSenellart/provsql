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
  bool isnull;
  Datum result;
  char nulls[1]={' '};

  HeapTuple tuple;
  
  Datum arguments[1]={token};
  Oid argtypes[1];
  
  if(PG_ARGISNULL(1))
    PG_RETURN_NULL();

  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  argtypes[0]=constants.OID_TYPE_VARCHAR;

  SPI_connect();

  if(SPI_execute_with_args(
        "SELECT val FROM aggregation_circuit_extra WHERE gate=$1",
        1,
        argtypes,
        arguments,
        nulls,
        true,
        1) != SPI_OK_SELECT) {
    elog(ERROR, "Cannot get aggregation value from aggregation_circuit_extra");
  }
  
  tuple = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(tuple, 1, SPI_tuptable->tupdesc, &isnull);

  SPI_finish();

  if(isnull)
    PG_RETURN_NULL();
  else
    PG_RETURN_DATUM(result);
/* 
    // char    *str = PG_GETARG_VARCHAR_P(0);
    // char       *result;
    // //TODO add SQL request to show the value
    // result = psprintf("%s (*)", str);
    // PG_RETURN_CSTRING(result); */
}