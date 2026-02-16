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
  const unsigned toklen=sizeof(result->tok)-1;
  unsigned vallen;

  result = (agg_token *)palloc(sizeof(agg_token));

  // str is ( UUID , string ) with UUID starting at 2 and with length
  // 20 (2*UUID-LEN=16) plus 4 hashes; then three characters we can
  // ignore (two spaces and comma) then the string then two ignored
  // spaces at the end
  if(strlen(str)<toklen+7 ||
     str[0]!='(' || str[1]!=' ' || str[2+toklen] != ' ' || str[2+toklen+1] != ','
     || str[2+toklen+2] != ' ' || str[strlen(str)-2] != ' '
     || str[strlen(str)-1] != ')')
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for agg_token: \"%s\"",
                    str)));

  strncpy(result->tok, str+2, toklen);
  result->tok[toklen]='\0';

  vallen=strlen(str)-toklen-2-3-2;
  if(vallen>=sizeof(result->val))
    vallen=sizeof(result->val)-1;
  strncpy(result->val, str+2+toklen+3, vallen);
  result->val[vallen]='\0';

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
