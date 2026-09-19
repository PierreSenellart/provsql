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
#include "utils/numeric.h"
#include "utils/fmgrprotos.h"
#include "utils/builtins.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "access/htup_details.h"

#include "provsql_utils.h"
#include "agg_token.h"

/**
 * @brief Set the value of @p aggtok to the @p len first bytes of @p val.
 *
 * A value too long for @c agg_token::val keeps its first
 * @c AGG_TOKEN_PREFIX_LEN bytes, marked by @c AGG_TOKEN_TRUNCATED in the last
 * byte; @p aggtok must be zeroed.
 */
void agg_token_set_value(agg_token *aggtok, const char *val, size_t len)
{
  if(len < sizeof(aggtok->val) - 1) {
    memcpy(aggtok->val, val, len);
    aggtok->val[len] = '\0';
  } else {
    memcpy(aggtok->val, val, AGG_TOKEN_PREFIX_LEN);
    aggtok->val[AGG_TOKEN_PREFIX_LEN] = '\0';
    aggtok->val[sizeof(aggtok->val) - 1] = AGG_TOKEN_TRUNCATED;
  }
}

/**
 * @brief The value of @p aggtok, as a C string.
 *
 * The value stored in the token, or, when it is only a prefix, the value of
 * its gate, read with @c provsql.agg_token_value_text; the prefix is returned
 * if the gate gives none that starts with it.
 */
const char *agg_token_value_cstring(const agg_token *aggtok)
{
  MemoryContext caller = CurrentMemoryContext;
  Oid argtypes[1] = {TEXTOID};
  Datum args[1];
  char *full = NULL;

  if(aggtok->val[sizeof(aggtok->val) - 1] != AGG_TOKEN_TRUNCATED ||
     strlen(aggtok->val) != AGG_TOKEN_PREFIX_LEN)
    return aggtok->val;

  args[0] = CStringGetTextDatum(aggtok->tok);
  if(SPI_connect() != SPI_OK_CONNECT)
    return aggtok->val;
  if(SPI_execute_with_args("SELECT provsql.agg_token_value_text($1::uuid)",
                           1, argtypes, args, NULL, true, 1) == SPI_OK_SELECT &&
     SPI_processed == 1) {
    char *v = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    size_t n;
    /* agg_token_value_text gives the value followed by " (*)"; it is that
     * of the token if it starts with the prefix the token holds. */
    if(v != NULL && (n = strlen(v)) >= AGG_TOKEN_PREFIX_LEN + 4 &&
       strcmp(v + n - 4, " (*)") == 0 &&
       strncmp(v, aggtok->val, AGG_TOKEN_PREFIX_LEN) == 0) {
      v[n - 4] = '\0';
      full = MemoryContextStrdup(caller, v);
    }
  }
  SPI_finish();
  return full != NULL ? full : aggtok->val;
}

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

  result = (agg_token *)palloc0(sizeof(agg_token));

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
  agg_token_set_value(result, str+2+toklen+3, vallen);

  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(agg_token_out);
/**
 * @brief Produce a display string for an @c agg_token.
 *
 * Default: returns @c "value (*)" (the running value followed by
 * @c " (*)"), matching @c EXPLAIN and direct @c CAST to text.
 *
 * When the @c provsql.aggtoken_text_as_uuid GUC is on, returns the
 * underlying provenance UUID instead. This is the form ProvSQL
 * Studio enables per session so agg_token cells in a result table
 * expose the circuit root UUID for click-through; the user-facing
 * @c "value (*)" string is recovered via the
 * @c provsql.agg_token_value_text(uuid) helper.
 *
 * @return C-string representation of the agg_token.
 */
Datum
agg_token_out(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  char *result;

  if (provsql_aggtoken_text_as_uuid)
    result = psprintf("%s", aggtok->tok);
  else
    result = psprintf("%s (*)", agg_token_value_cstring(aggtok));

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

/**
 * @brief True when an @c agg_token value string cannot carry a number.
 *
 * An empty or literal @c "NULL" value string (an aggregate whose value is
 * SQL NULL) must convert to SQL NULL rather than feed the type-input
 * function a string it rejects.
 */
static bool
agg_token_val_is_null(const agg_token *aggtok)
{
  return aggtok->val[0] == '\0' || strcmp(aggtok->val, "NULL") == 0;
}

PG_FUNCTION_INFO_V1(agg_token_to_numeric);
/**
 * @brief Cast an @c agg_token to @c numeric, extracting only the value.
 *
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Numeric datum parsed from the aggregate value string.
 */
Datum
agg_token_to_numeric(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  Datum result;

  provsql_warning("converting agg_token to numeric: provenance information is lost");

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  result = DirectFunctionCall3(numeric_in,
                               CStringGetDatum(agg_token_value_cstring(aggtok)),
                               ObjectIdGetDatum(InvalidOid),
                               Int32GetDatum(-1));
  PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(agg_token_value);
/**
 * @brief Extract the running value of an @c agg_token as @c numeric.
 *
 * Unlike @c agg_token_to_numeric, this does @b not emit the
 * "provenance information is lost" warning: it is the internal accessor
 * used by the provenance-preserving @c agg_token arithmetic operators
 * (which keep the provenance in a @c gate_arith circuit), where no
 * provenance is dropped.
 * @return Numeric datum parsed from the aggregate value string.
 */
Datum
agg_token_value(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  Datum result;

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  result = DirectFunctionCall3(numeric_in,
                               CStringGetDatum(agg_token_value_cstring(aggtok)),
                               ObjectIdGetDatum(InvalidOid),
                               Int32GetDatum(-1));
  PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(agg_token_to_float8);
/**
 * @brief Cast an @c agg_token to @c double precision, extracting only the value.
 *
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Float8 datum parsed from the aggregate value string.
 */
Datum
agg_token_to_float8(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  Datum result;

  provsql_warning("converting agg_token to double precision: provenance information is lost");

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  result = DirectFunctionCall1(float8in,
                               CStringGetDatum(agg_token_value_cstring(aggtok)));
  PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(agg_token_to_int4);
/**
 * @brief Cast an @c agg_token to @c integer, extracting only the value.
 *
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Int32 datum parsed from the aggregate value string.
 */
Datum
agg_token_to_int4(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  Datum result;

  provsql_warning("converting agg_token to integer: provenance information is lost");

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  result = DirectFunctionCall1(int4in,
                               CStringGetDatum(agg_token_value_cstring(aggtok)));
  PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(agg_token_to_int8);
/**
 * @brief Cast an @c agg_token to @c bigint, extracting only the value.
 *
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Int64 datum parsed from the aggregate value string.
 */
Datum
agg_token_to_int8(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  Datum result;

  provsql_warning("converting agg_token to bigint: provenance information is lost");

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  result = DirectFunctionCall1(int8in,
                               CStringGetDatum(agg_token_value_cstring(aggtok)));
  PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(agg_token_to_bool);
/**
 * @brief Cast an @c agg_token to @c boolean, extracting only the value.
 *
 * The value of a boolean aggregate (@c bool_or, @c bool_and, @c every).
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Boolean datum parsed from the aggregate value string.
 */
Datum
agg_token_to_bool(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);

  provsql_warning("converting agg_token to boolean: provenance information is lost");

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();

  return DirectFunctionCall1(boolin, CStringGetDatum(agg_token_value_cstring(aggtok)));
}

PG_FUNCTION_INFO_V1(agg_token_to_text);
/**
 * @brief Cast an @c agg_token to @c text, extracting only the value.
 *
 * Unlike @c agg_token_cast which returns the UUID part, this returns
 * the aggregate value as text.
 * Emits a WARNING that provenance information is lost during the conversion.
 * @return Text datum containing the aggregate value string.
 */
Datum
agg_token_to_text(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);
  text *txt_result;
  const char *val;
  int len;

  provsql_warning("converting agg_token to text: provenance information is lost");

  val = agg_token_value_cstring(aggtok);
  len = strlen(val);
  txt_result = (text *) palloc(len + VARHDRSZ);
  SET_VARSIZE(txt_result, len + VARHDRSZ);
  memcpy(VARDATA(txt_result), val, len);

  PG_RETURN_TEXT_P(txt_result);
}

PG_FUNCTION_INFO_V1(agg_token_plain_text);
/**
 * @brief The value of an @c agg_token as @c text, NULL for a NULL value,
 *        without the "provenance information is lost" warning.
 *
 * The internal accessor of the rewriter's sort keys: an @c ORDER @c BY on an
 * aggregate result sorts on this value, read in the aggregate's type, while
 * the column itself keeps its @c agg_token.
 */
Datum
agg_token_plain_text(PG_FUNCTION_ARGS)
{
  agg_token *aggtok = (agg_token *) PG_GETARG_POINTER(0);

  if (agg_token_val_is_null(aggtok))
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(agg_token_value_cstring(aggtok)));
}

PG_FUNCTION_INFO_V1(row_number_as_rank);
/**
 * @brief The @c agg_token of @c rank() standing for @c row_number().
 *
 * The two are equal when the @c ORDER @c BY of the window leaves no ties;
 * with ties, SQL itself does not determine which row gets which number.
 * The rank is what is tracked: when @p row_number differs from it, a warning
 * says so, once per statement.
 *
 * @param rank        The @c agg_token of the rank of the row.
 * @param row_number  The row number PostgreSQL gave the row.
 * @return @p rank.
 */
Datum
row_number_as_rank(PG_FUNCTION_ARGS)
{
  static TimestampTz warned = 0;
  agg_token *rank = (agg_token *) PG_GETARG_POINTER(0);
  int64 row_number = PG_GETARG_INT64(1);

  if (!agg_token_val_is_null(rank) &&
      strtoll(rank->val, NULL, 10) != row_number &&
      warned != GetCurrentStatementStartTimestamp()) {
    warned = GetCurrentStatementStartTimestamp();
    provsql_warning("row_number() / LIMIT / DISTINCT ON is tracked as rank() "
                    "(WITH TIES), which it differs from when rows tie on the "
                    "ORDER BY");
  }
  PG_RETURN_POINTER(rank);
}
