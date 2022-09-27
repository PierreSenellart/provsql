#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "access/htup_details.h"

#include "provsql_utils.h"
#include "provsql_shmem.h"

PG_FUNCTION_INFO_V1(aggregation_evaluate);

Datum aggregation_evaluate(PG_FUNCTION_ARGS)
{
  Datum token = PG_GETARG_DATUM(0);
  Datum token2value = PG_GETARG_DATUM(1);
  Datum agg_function_final = PG_GETARG_DATUM(2);
  Datum agg_function = PG_GETARG_DATUM(3);
  Datum semimod_function = PG_GETARG_DATUM(4);
  Oid element_type = get_fn_expr_argtype(fcinfo->flinfo, 5);
  Datum element_one = PG_ARGISNULL(5)?((Datum) 0):PG_GETARG_DATUM(5);
  Datum plus_function = PG_GETARG_DATUM(6);
  Datum times_function = PG_GETARG_DATUM(7);
  Datum monus_function = PG_GETARG_DATUM(8);
  Datum delta_function = PG_GETARG_DATUM(9);
  const constants_t constants = initialize_constants(true);

  bool isnull;
  Datum result;
  char nulls[11]={' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};

  HeapTuple tuple;
  
  Datum arguments[11]={token,token2value,agg_function_final,agg_function,semimod_function,element_one,element_type,plus_function,
    times_function,monus_function,delta_function};
  Oid argtypes[11];
  
  if(PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) || PG_ARGISNULL(4)
      || PG_ARGISNULL(5) || PG_ARGISNULL(6) || PG_ARGISNULL(7))
    PG_RETURN_NULL();

  if(PG_ARGISNULL(8)) // No monus function provided
    nulls[9]='n';

  if(PG_ARGISNULL(9)) // No delta function provided
    nulls[10]='n';

  argtypes[0]=constants.OID_TYPE_UUID;
  argtypes[1]=REGCLASSOID;
  argtypes[2]=REGPROCOID;
  argtypes[3]=REGPROCOID; 
  argtypes[4]=REGPROCOID;
  argtypes[5]=element_type;
  argtypes[6]=REGTYPEOID;
  argtypes[7]=REGPROCOID;
  argtypes[8]=REGPROCOID;
  argtypes[9]=REGPROCOID;
  argtypes[10]=REGPROCOID;

  SPI_connect();

  if(SPI_execute_with_args(
        "SELECT provsql.aggregation_evaluate($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)",
        11,
        argtypes,
        arguments,
        nulls,
        true,
        1) != SPI_OK_SELECT) {
    elog(ERROR, "Cannot execute real aggregation_evaluate function");
  }
  
  tuple = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(tuple, 1, SPI_tuptable->tupdesc, &isnull);

  SPI_finish();

  if(isnull)
    PG_RETURN_NULL();
  else
    PG_RETURN_DATUM(result);
}
