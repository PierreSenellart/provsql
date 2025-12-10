#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

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

PG_FUNCTION_INFO_V1(agg_token_numeric);
Datum
agg_token_numeric(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  
  return DirectFunctionCall3(numeric_in,
                             CStringGetDatum((char *) aggtok->val),
                             ObjectIdGetDatum(InvalidOid),
                             Int32GetDatum(-1));
}

PG_FUNCTION_INFO_V1(agg_token_float8);
Datum
agg_token_float8(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  
  return DirectFunctionCall1(float8in,
                             CStringGetDatum((char *) aggtok->val));
}

PG_FUNCTION_INFO_V1(agg_token_int4);
Datum
agg_token_int4(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  
  return DirectFunctionCall1(int4in,
                             CStringGetDatum((char *) aggtok->val));
}

PG_FUNCTION_INFO_V1(agg_token_int8);
Datum
agg_token_int8(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  
  return DirectFunctionCall1(int8in,
                             CStringGetDatum((char *) aggtok->val));
}
