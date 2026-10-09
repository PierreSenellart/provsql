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
#include "catalog/pg_collation.h"
#include "utils/uuid.h"
#include "utils/numeric.h"
#include "utils/fmgrprotos.h"
#include "utils/builtins.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "access/htup_details.h"
#include "utils/datum.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"

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
 * @c " (*)"), matching @c EXPLAIN and direct @c CAST to text.  Where the value
 * is SQL NULL it is @c "(*)" alone: SQL writes nothing for a NULL, so writing
 * @c "NULL" would put four characters where a reader expects none, and a text
 * client would read them as data.
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
  else if (strcmp(aggtok->val, "NULL") == 0)
    /* The value is SQL NULL here -- an aggregate with no value, or one the
     * arithmetic has none for (a division whose divisor reads zero, which
     * ProvSQL answers rather than raising so that the other worlds stay
     * answerable).  SQL never writes the four characters NULL, so neither do
     * we: the marker alone says the row is tracked and has no value here.
     *
     * Only that spelling, NOT the empty value string @c agg_token_val_is_null
     * also accepts: an aggregate of the empty text has a value, and writing it
     * as though it had none would lose the difference.  The two are already
     * indistinguishable in what the gate records, which is a separate matter
     * from printing them alike. */
    result = pstrdup("(*)");
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

/**
 * @brief The "provenance information is lost" warning of a conversion, once
 *        for a statement and for each target type.
 *
 * A conversion runs for every row, and so did its warning: a query over a
 * large relation wrote one line per row, which says nothing the first does not
 * and which has filled a server log before.  The statement is identified by
 * @c provsql_stmt_serial, which the executor hook bumps for the user's
 * outermost statement; a conversion outside any statement of the user (a
 * direct call in a function the rewriting runs) warns once as well.
 */
static void warn_conversion_once(const char *target) {
  /* One remembered serial per target type: the list is short and fixed. */
  static const char *seen_target[8];
  static unsigned seen_serial[8];
  static int nseen = 0;
  int i;

  /* Tagged like a freezing, and scoped deliberate, although nothing is frozen
   * here: this is the conversion of a token the RELATION holds -- reading
   * "x::text" of a column an earlier statement wrote -- and the text of a
   * token is the value on the data as it is, which no rewriting gives a
   * provenance to.  The tag is its own, not the freezing's
   * (aggregate-read-as-plain-value), so that a survey counting causes does not
   * take a read of stored data for a fragment ProvSQL failed to track. */
  for (i = 0; i < nseen; ++i)
    if (seen_target[i] == target) {
      if (seen_serial[i] == provsql_stmt_serial)
        return;
      seen_serial[i] = provsql_stmt_serial;
      provsql_warning_tagged(PROVSQL_DELIBERATE, "stored-agg-token-conversion", "converting agg_token to %s: provenance information is "
                             "lost", target);
      return;
    }
  if (nseen < (int)(sizeof(seen_target) / sizeof(seen_target[0]))) {
    seen_target[nseen] = target;
    seen_serial[nseen] = provsql_stmt_serial;
    ++nseen;
  }
  provsql_warning_tagged(PROVSQL_DELIBERATE, "stored-agg-token-conversion", "converting agg_token to %s: provenance information is lost",
                         target);
}


/** @brief The "sorted on the plain value" warning, once for a statement. */
static void warn_ordering_once(void)
{
  static unsigned seen = (unsigned)-1;

  if (seen == provsql_stmt_serial)
    return;
  seen = provsql_stmt_serial;
  provsql_warning_tagged(PROVSQL_DELIBERATE, "agg-token-ordered-by-value",
                         "ordering or grouping an aggregate result reads "
                         "the value shown, not per world");
}

/** @brief Whether @p v is the text of a number, as @c numeric_in would read
 *  it: the check a comparison makes before parsing, since parsing what is not
 *  one would raise where a comparison must not. */
static bool text_is_number(const char *v)
{
  const char *p = v;
  bool digits = false;

  if (p == NULL)
    return false;
  while (*p == ' ') ++p;
  if (*p == '+' || *p == '-') ++p;
  while (*p >= '0' && *p <= '9') { ++p; digits = true; }
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') { ++p; digits = true; }
  }
  if (digits && (*p == 'e' || *p == 'E')) {
    ++p;
    if (*p == '+' || *p == '-') ++p;
    if (!(*p >= '0' && *p <= '9')) return false;
    while (*p >= '0' && *p <= '9') ++p;
  }
  while (*p == ' ') ++p;
  return digits && *p == '\0';
}

PG_FUNCTION_INFO_V1(agg_token_btree_cmp);
/**
 * @brief Order two @c agg_token values by the value each carries.
 *
 * An @c agg_token is a value and a provenance, and the value is the one the
 * data as it is gives: ordering by it -- @c ORDER @c BY, @c DISTINCT, a
 * @c GROUP @c BY that sorts -- is ordering by that one value, which another
 * possible world need not agree with.  The warning says so, once for the
 * statement rather than once per comparison, since a sort makes n log n of
 * them.
 *
 * A token with no value (an aggregate over no row) sorts first, as a @c NULL
 * does under @c NULLS @c FIRST; two numbers are compared as numbers and
 * anything else by its text, so that the order is total whatever the
 * aggregate's own type.
 */
Datum
agg_token_btree_cmp(PG_FUNCTION_ARGS)
{
  agg_token *a = (agg_token *) PG_GETARG_POINTER(0);
  agg_token *b = (agg_token *) PG_GETARG_POINTER(1);
  bool a_null = agg_token_val_is_null(a), b_null = agg_token_val_is_null(b);
  const char *av, *bv;

  warn_ordering_once();

  if (a_null || b_null)
    PG_RETURN_INT32(a_null && b_null ? 0 : (a_null ? -1 : 1));
  av = agg_token_value_cstring(a);
  bv = agg_token_value_cstring(b);
  if (text_is_number(av) && text_is_number(bv)) {
    Datum na = DirectFunctionCall3(numeric_in, CStringGetDatum(av),
                                   ObjectIdGetDatum(InvalidOid),
                                   Int32GetDatum(-1));
    Datum nb = DirectFunctionCall3(numeric_in, CStringGetDatum(bv),
                                   ObjectIdGetDatum(InvalidOid),
                                   Int32GetDatum(-1));
    PG_RETURN_INT32(DatumGetInt32(DirectFunctionCall2(numeric_cmp, na, nb)));
  }
  {
    int c = strcmp(av, bv);
    PG_RETURN_INT32(c < 0 ? -1 : (c > 0 ? 1 : 0));
  }
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

  warn_conversion_once("numeric");

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

  warn_conversion_once("double precision");

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

  warn_conversion_once("integer");

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

  warn_conversion_once("bigint");

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

  warn_conversion_once("boolean");

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

  warn_conversion_once("text");

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

PG_FUNCTION_INFO_V1(ntile_as_rank);
/**
 * @brief The @c agg_token of the bucket @c ntile() gives a row, read over
 *        its rank.
 *
 * Rows that tie on the @c ORDER @c BY of the window share the bucket of
 * their rank, which is the reading of the semantics; SQL numbers them in
 * some order and may split them between two buckets.  Its SQL arguments
 * are @c bucket, the @c agg_token of the tracked bucket, and @c sql_bucket,
 * the bucket PostgreSQL gave the row; when the two differ, a warning says so,
 * once per statement.
 *
 * @return @c bucket.
 */
Datum
ntile_as_rank(PG_FUNCTION_ARGS)
{
  static TimestampTz warned = 0;
  agg_token *bucket = (agg_token *) PG_GETARG_POINTER(0);
  int64 sql_bucket = PG_GETARG_INT64(1);

  if (!agg_token_val_is_null(bucket) &&
      strtoll(bucket->val, NULL, 10) != sql_bucket &&
      warned != GetCurrentStatementStartTimestamp()) {
    warned = GetCurrentStatementStartTimestamp();
    provsql_warning_tagged(PROVSQL_DELIBERATE, "ntile-as-rank",
                           "ntile() gives rows that tie on the ORDER BY the "
                           "bucket of their rank, where SQL splits them "
                           "between buckets");
  }
  PG_RETURN_POINTER(bucket);
}

PG_FUNCTION_INFO_V1(row_number_as_rank);
/**
 * @brief The @c agg_token of @c rank() standing for @c row_number().
 *
 * The two are equal when the @c ORDER @c BY of the window leaves no ties;
 * with ties, SQL itself does not determine which row gets which number.
 * The rank is what is tracked.  Its SQL arguments are @c rank, the
 * @c agg_token of the rank of the row, and @c row_number, the row number of
 * the row among those of the database as it is, or NULL for a row absent
 * from it; when the two differ, or when @c tied says the row ties with a row
 * of any world, a warning says so, once per statement.
 *
 * @return @c rank.
 */
Datum
row_number_as_rank(PG_FUNCTION_ARGS)
{
  static TimestampTz warned = 0;
  agg_token *rank;
  int64 row_number;

  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  rank = (agg_token *) PG_GETARG_POINTER(0);
  /* A tie with a row of any world, those absent from the database as it is
   * included: the row number SQL would give is not determined there. */
  if (PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2) &&
      warned != GetCurrentStatementStartTimestamp()) {
    warned = GetCurrentStatementStartTimestamp();
    provsql_warning_tagged(PROVSQL_DELIBERATE, "row-number-as-rank",
                           "row_number() / LIMIT / DISTINCT ON is tracked as "
                           "rank() (WITH TIES), which it differs from when "
                           "rows tie on the ORDER BY");
  }
  /* No row number: a row absent from the database as it is, which SQL does
   * not number (see make_rank_expression). */
  if (PG_ARGISNULL(1))
    PG_RETURN_POINTER(rank);
  row_number = PG_GETARG_INT64(1);

  if (!agg_token_val_is_null(rank) &&
      strtoll(rank->val, NULL, 10) != row_number &&
      warned != GetCurrentStatementStartTimestamp()) {
    warned = GetCurrentStatementStartTimestamp();
    provsql_warning_tagged(PROVSQL_DELIBERATE, "row-number-as-rank",
                           "row_number() / LIMIT / DISTINCT ON is tracked as "
                           "rank() (WITH TIES), which it differs from when "
                           "rows tie on the ORDER BY");
  }
  PG_RETURN_POINTER(rank);
}

/** @brief Transition state of @c order_determined. */
typedef struct order_determined_state {
  int nvals;            ///< Number of aggregated arguments
  int nargs;            ///< Aggregated arguments, then the sort keys
  bool have_prev;       ///< A row was read
  bool determined;      ///< No two peers so far with different values
  Datum *prev;          ///< The previous row's arguments
  bool *prevnull;       ///< Which of them are NULL
  int16 *typlen;        ///< Their type lengths
  bool *typbyval;       ///< Whether passed by value
  FmgrInfo **keyeq;     ///< Equality of each sort key (NULL for a value)
} order_determined_state;

/** @brief Whether two values of the same type are the same value, written the
 *  same way: equal bytes, after decompression for a varlena. */
static bool
same_value(Datum a, Datum b, int16 typlen, bool typbyval)
{
  if (typlen == -1) {
    struct varlena *va = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(a));
    struct varlena *vb = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(b));
    Size la = VARSIZE_ANY_EXHDR(va), lb = VARSIZE_ANY_EXHDR(vb);

    return la == lb && memcmp(VARDATA_ANY(va), VARDATA_ANY(vb), la) == 0;
  }
  return datumIsEqual(a, b, typbyval, typlen);
}

PG_FUNCTION_INFO_V1(order_determined_transfn);
/**
 * @brief Transition of @c order_determined(nvals, v_1, ..., v_nvals,
 *        k_1, ..., k_m): reads the rows of a group in the order of its sort
 *        keys @c k, and records whether two consecutive rows tie on the keys
 *        (are peers) with different values @c v.
 *
 * The aggregate it accompanies reads its rows in an order the query then
 * does not determine.  Without sort keys, every row is a peer of every
 * other.  Two NULLs are peers, as they sort together.
 */
Datum
order_determined_transfn(PG_FUNCTION_ARGS)
{
  MemoryContext aggcxt, old;
  order_determined_state *st;
  int i;
  bool peers = true, same = true;

  if (!AggCheckCallContext(fcinfo, &aggcxt))
    elog(ERROR, "order_determined_transfn called in non-aggregate context");

  if (PG_ARGISNULL(0)) {
    old = MemoryContextSwitchTo(aggcxt);
    st = palloc0(sizeof(order_determined_state));
    st->nvals = PG_GETARG_INT32(1);
    st->nargs = PG_NARGS() - 2;
    st->determined = true;
    st->prev = palloc0(st->nargs * sizeof(Datum));
    st->prevnull = palloc0(st->nargs * sizeof(bool));
    st->typlen = palloc0(st->nargs * sizeof(int16));
    st->typbyval = palloc0(st->nargs * sizeof(bool));
    st->keyeq = palloc0(st->nargs * sizeof(FmgrInfo *));
    for (i = 0; i < st->nargs; ++i) {
      Oid t = get_fn_expr_argtype(fcinfo->flinfo, i + 2);
      get_typlenbyval(t, &st->typlen[i], &st->typbyval[i]);
      if (i >= st->nvals) {
        TypeCacheEntry *tce = lookup_type_cache(t, TYPECACHE_EQ_OPR_FINFO);
        if (OidIsValid(tce->eq_opr_finfo.fn_oid))
          st->keyeq[i] = &tce->eq_opr_finfo;
      }
    }
    MemoryContextSwitchTo(old);
  } else
    st = (order_determined_state *) PG_GETARG_POINTER(0);

  if (!st->determined)
    PG_RETURN_POINTER(st);

  if (st->have_prev) {
    for (i = st->nvals; i < st->nargs && peers; ++i) {
      bool n = PG_ARGISNULL(i + 2);
      if (n != st->prevnull[i])
        peers = false;
      else if (!n)
        peers = st->keyeq[i] != NULL
          ? DatumGetBool(FunctionCall2Coll(st->keyeq[i],
                                           OidIsValid(PG_GET_COLLATION())
                                             ? PG_GET_COLLATION()
                                             : DEFAULT_COLLATION_OID,
                                           st->prev[i], PG_GETARG_DATUM(i + 2)))
          : same_value(st->prev[i], PG_GETARG_DATUM(i + 2), st->typlen[i],
                       st->typbyval[i]);
    }
    if (peers) {
      for (i = 0; i < st->nvals && same; ++i) {
        bool n = PG_ARGISNULL(i + 2);
        if (n != st->prevnull[i])
          same = false;
        else if (!n)
          same = same_value(st->prev[i], PG_GETARG_DATUM(i + 2),
                            st->typlen[i], st->typbyval[i]);
      }
      if (!same) {
        st->determined = false;
        PG_RETURN_POINTER(st);
      }
    }
  }

  old = MemoryContextSwitchTo(aggcxt);
  for (i = 0; i < st->nargs; ++i) {
    if (st->have_prev && !st->prevnull[i] && !st->typbyval[i])
      pfree(DatumGetPointer(st->prev[i]));
    st->prevnull[i] = PG_ARGISNULL(i + 2);
    st->prev[i] = st->prevnull[i]
      ? (Datum) 0
      : datumCopy(PG_GETARG_DATUM(i + 2), st->typbyval[i], st->typlen[i]);
  }
  MemoryContextSwitchTo(old);
  st->have_prev = true;
  PG_RETURN_POINTER(st);
}

PG_FUNCTION_INFO_V1(order_determined_finalfn);
/** @brief Final function of @c order_determined: whether no two peers have
 *  different values. */
Datum
order_determined_finalfn(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0))
    PG_RETURN_BOOL(true);
  PG_RETURN_BOOL(((order_determined_state *) PG_GETARG_POINTER(0))->determined);
}

PG_FUNCTION_INFO_V1(order_checked);
/**
 * @brief The value of an order-dependent aggregate, with a warning, once per
 *        statement, where @p determined says its order is not determined.
 *
 * The rows tie on its @c ORDER @c BY (or it has none) with different
 * values: SQL leaves their order open, and the order of the database as it
 * is is the one read in every world.
 *
 * @return its first argument.
 */
Datum
order_checked(PG_FUNCTION_ARGS)
{
  static TimestampTz warned = 0;

  if (!PG_ARGISNULL(1) && !PG_GETARG_BOOL(1) &&
      warned != GetCurrentStatementStartTimestamp()) {
    warned = GetCurrentStatementStartTimestamp();
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2))
      provsql_warning_tagged(PROVSQL_DELIBERATE, "window-order-undetermined",
                             "a window function reads rows whose order the "
                             "query does not determine (lag or lead with "
                             "rows tying on its ORDER BY, first_value, "
                             "last_value or nth_value with tying rows of "
                             "different values): the order of the database "
                             "as it is is read in every world");
    else
    provsql_warning_tagged(PROVSQL_DELIBERATE, "aggregate-order-undetermined",
                           "an order-dependent aggregate reads rows whose "
                           "order the query does not determine (without "
                           "ORDER BY, or tying on it, with different values): "
                           "their order in the database as it is is read in "
                           "every world");
  }
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();
  PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}
