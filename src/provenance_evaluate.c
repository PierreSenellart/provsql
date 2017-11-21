#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "access/htup_details.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance_evaluate);

Datum provenance_evaluate(PG_FUNCTION_ARGS)
{
  Datum token = PG_GETARG_DATUM(0);
  Datum token2value = PG_GETARG_DATUM(1);
  Oid element_type = get_fn_expr_argtype(fcinfo->flinfo, 2);
  Datum element_one = PG_ARGISNULL(2)?((Datum) 0):PG_GETARG_DATUM(2);
  Datum plus_function = PG_GETARG_DATUM(3);
  Datum times_function = PG_GETARG_DATUM(4);
  Datum monus_function = PG_GETARG_DATUM(5);

  constants_t constants;
  bool isnull;
  Datum result;
  char nulls[7]={' ',' ',' ',' ',' ',' ',' '};

  HeapTuple tuple;
  
  Datum arguments[7]={token,token2value,element_one,element_type,plus_function,times_function,monus_function};
  Oid argtypes[7];
  
  if(PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) || PG_ARGISNULL(4))
    PG_RETURN_NULL();

  if(PG_ARGISNULL(5)) // No monus function provided
    nulls[6]='n';

  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  argtypes[0]=constants.OID_TYPE_PROVENANCE_TOKEN;
  argtypes[1]=REGCLASSOID;
  argtypes[2]=element_type;
  argtypes[3]=REGTYPEOID;
  argtypes[4]=REGPROCOID;
  argtypes[5]=REGPROCOID;
  argtypes[6]=REGPROCOID;

  SPI_connect();

  if(SPI_execute_with_args(
        "SELECT provsql.provenance_evaluate($1,$2,$3,$4,$5,$6,$7)",
        7,
        argtypes,
        arguments,
        nulls,
        true,
        1) != SPI_OK_SELECT) {
    elog(ERROR, "Cannot execute real provenance_evaluate function");
  }
  
  tuple = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(tuple, 1, SPI_tuptable->tupdesc, &isnull);

  SPI_finish();

  if(isnull)
    PG_RETURN_NULL();
  else
    PG_RETURN_DATUM(result);
}
