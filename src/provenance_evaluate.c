#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"

#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance_evaluate);

Datum provenance_evaluate(PG_FUNCTION_ARGS)
{
  Datum token = PG_GETARG_DATUM(0);
  Datum token2value = PG_GETARG_DATUM(1);
  Oid element_type = get_fn_expr_argtype(fcinfo->flinfo, 2);
  Datum element_one = PG_ARGISNULL(2)?((Datum) 0):PG_GETARG_DATUM(2);
  Datum or_function = PG_GETARG_DATUM(3);
  Datum and_function = PG_GETARG_DATUM(4);

  constants_t constants;
  bool isnull;
  Datum result;
  
  Datum arguments[6]={token,token2value,element_one,element_type,or_function,and_function};
  Oid argtypes[6];
  
  if(PG_ARGISNULL(2) || PG_ARGISNULL(3) || PG_ARGISNULL(4))
    PG_RETURN_NULL();

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    PG_RETURN_DATUM(element_one);

  initialize_constants(&constants);

  argtypes[0]=constants.PROVENANCE_TOKEN_OID;
  argtypes[1]=REGCLASSOID;
  argtypes[2]=element_type;
  argtypes[3]=REGTYPEOID;
  argtypes[4]=REGPROCOID;
  argtypes[5]=REGPROCOID;

  SPI_connect();

  if(SPI_execute_with_args(
        "SELECT provenance_evaluate($1,$2,$3,$4,$5,$6)",
        6,
        argtypes,
        arguments,
        NULL,
        true,
        1) != SPI_OK_SELECT) {
    elog(ERROR, "Cannot execute real provenance_evaluate function");
  }
  
  result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
  
  SPI_finish();

  if(isnull)
    PG_RETURN_NULL();
  else
    PG_RETURN_DATUM(result);
}
