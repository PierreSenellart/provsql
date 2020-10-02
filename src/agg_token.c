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
    char *str = PG_GETARG_CSTRING(0);
    agg_token* result;

    result = (agg_token *)palloc(sizeof(agg_token));

     if(sscanf(str, "( %s , %s )", result->tok, result->val) != 2)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
               errmsg("invalid input syntax for agg_token: \"%s\"",
                      str)));

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(agg_token_out);
Datum
agg_token_out(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  char *result;

  result = psprintf("%s (*)", aggtok->val);

  PG_RETURN_CSTRING(result);

  /* no need for now
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
  */
}

PG_FUNCTION_INFO_V1(agg_token_cast);
Datum
agg_token_cast(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  char *result;
  text *txt_result;
  int len;

  result = psprintf("%s", aggtok->tok);
  len = strlen(result);

  txt_result = (text *) palloc(len + ((int32) sizeof(int32)));
 
  SET_VARSIZE(txt_result, len +   ((int32) sizeof(int32)));
  memcpy(VARDATA(txt_result), result, len);

  PG_RETURN_TEXT_P(txt_result);
}