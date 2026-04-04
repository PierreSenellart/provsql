/**
 * @file agg_token.c
 * @brief PostgreSQL I/O functions and cast for the @c agg_token composite type.
 *
 * Implements the three SQL-callable C functions that back the
 * @c agg_token type:
 * - @c agg_token_in()   – text → agg_token (input function)
 * - @c agg_token_out()  – agg_token → text (output function)
 * - @c agg_token_cast() – agg_token → text (cast, extracts the UUID part)
 *
 * The on-wire text format is @c ( UUID , value ) where @c UUID is the
 * 36-character hyphenated UUID of the provenance gate and @c value is
 * the aggregate running value.
 */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "access/htup_details.h"

#include "provsql_utils.h"
#include "agg_token.h"

PG_FUNCTION_INFO_V1(agg_token_in);
/**
 * @brief Parse an @c agg_token value from its text representation.
 *
 * Expected format: @c "( UUID , value )" with a single space around the
 * comma and at the outer parentheses.  Raises @c ERROR on malformed input.
 * @return Pointer to the newly allocated @c agg_token.
 */
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
/**
 * @brief Produce a human-readable display string for an @c agg_token.
 *
 * Returns @c "value (*)", i.e. the running value followed by @c " (*)".
 * This is the output used by @c EXPLAIN and direct @c CAST to text.
 * @return C-string representation of the agg_token value.
 */
Datum
agg_token_out(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  char *result;

  result = psprintf("%s (*)", aggtok->val);

  PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(agg_token_cast);
/**
 * @brief Cast an @c agg_token to @c text, returning only the UUID part.
 *
 * This is used when the caller needs the provenance circuit UUID
 * stored in the token rather than the aggregate value.
 * @return Text datum containing the UUID string of the token.
 */
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
