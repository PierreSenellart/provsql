/**
 * @file random_variable_type.c
 * @brief PostgreSQL I/O and constructor functions for the @c random_variable type.
 *
 * @c random_variable is a binary-internal type used in user-facing SQL
 * for continuous probabilistic c-tables.  It pairs a provenance circuit
 * UUID (the gate token) with a cached scalar value (used for
 * zero-variance constants produced by @c provsql.as_random).
 *
 * On-disk and on-wire text format: <tt>( UUID , value )</tt> with
 * single spaces around the comma and inside the outer parentheses,
 * mirroring the @c agg_token format.  The value is rendered by
 * @c float8out so that round-trip @c pg_dump / @c pg_restore preserves
 * the scalar exactly.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "utils/fmgrprotos.h"

/**
 * @brief Binary internal layout of @c random_variable.
 *
 * 16 bytes UUID + 8 bytes double = 24 bytes, double-aligned.
 * The @c val field caches the deterministic literal for
 * zero-variance constants and is @c NaN for actual distributions.
 */
typedef struct random_variable {
  pg_uuid_t tok;  ///< Provenance gate UUID
  double val;     ///< Cached scalar value (NaN for non-constant RVs)
} random_variable;

PG_FUNCTION_INFO_V1(random_variable_in);
/**
 * @brief Parse a @c random_variable from its text representation.
 *
 * Expected format: @c "( UUID , value )" where @c UUID is a
 * 36-character hyphenated UUID and @c value is parseable as
 * @c float8.  Raises @c ERROR on malformed input.
 */
Datum
random_variable_in(PG_FUNCTION_ARGS)
{
  /* Layout: '(' ' ' UUID(36) ' ' ',' ' ' value ' ' ')' */
  const size_t prefix_len = 2;        /* "( "                            */
  const size_t uuid_len   = 36;       /* xxxxxxxx-...-xxxxxxxxxxxx       */
  const size_t mid_len    = 3;        /* " , "                           */
  const size_t suffix_len = 2;        /* " )"                            */
  const size_t fixed_len  = prefix_len + uuid_len + mid_len + suffix_len;

  char *str = PG_GETARG_CSTRING(0);
  size_t len = strlen(str);
  char uuid_buf[37];
  Datum uuid_d;
  size_t value_start;
  size_t value_len;
  char *value_buf;
  Datum val_d;
  random_variable *result;

  if (len < fixed_len + 1
      || str[0] != '(' || str[1] != ' '
      || str[prefix_len + uuid_len]     != ' '
      || str[prefix_len + uuid_len + 1] != ','
      || str[prefix_len + uuid_len + 2] != ' '
      || str[len - 2] != ' '
      || str[len - 1] != ')')
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for random_variable: \"%s\"",
                    str)));

  memcpy(uuid_buf, str + prefix_len, uuid_len);
  uuid_buf[uuid_len] = '\0';
  uuid_d = DirectFunctionCall1(uuid_in, CStringGetDatum(uuid_buf));

  value_start = prefix_len + uuid_len + mid_len;
  value_len   = len - value_start - suffix_len;
  value_buf = (char *) palloc(value_len + 1);
  memcpy(value_buf, str + value_start, value_len);
  value_buf[value_len] = '\0';
  val_d = DirectFunctionCall1(float8in, CStringGetDatum(value_buf));
  pfree(value_buf);

  result = (random_variable *) palloc(sizeof(random_variable));
  memcpy(&result->tok, DatumGetUUIDP(uuid_d), sizeof(pg_uuid_t));
  result->val = DatumGetFloat8(val_d);
  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(random_variable_out);
/**
 * @brief Render a @c random_variable as @c "( UUID , value )" text.
 */
Datum
random_variable_out(PG_FUNCTION_ARGS)
{
  random_variable *rv = (random_variable *) PG_GETARG_POINTER(0);
  char *uuid_str;
  char *val_str;
  char *result;

  uuid_str = DatumGetCString(
      DirectFunctionCall1(uuid_out, UUIDPGetDatum(&rv->tok)));
  val_str = DatumGetCString(
      DirectFunctionCall1(float8out, Float8GetDatum(rv->val)));

  result = psprintf("( %s , %s )", uuid_str, val_str);

  pfree(uuid_str);
  pfree(val_str);
  PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(random_variable_uuid);
/**
 * @brief Extract the provenance UUID part of a @c random_variable.
 *
 * Used to back the implicit cast @c random_variable @c -> @c uuid so
 * @c provsql columns can pull the underlying gate token.
 */
Datum
random_variable_uuid(PG_FUNCTION_ARGS)
{
  random_variable *rv = (random_variable *) PG_GETARG_POINTER(0);
  pg_uuid_t *result = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
  memcpy(result, &rv->tok, sizeof(pg_uuid_t));
  PG_RETURN_UUID_P(result);
}

PG_FUNCTION_INFO_V1(random_variable_value);
/**
 * @brief Extract the cached scalar value of a @c random_variable.
 *
 * For zero-variance constants produced by @c provsql.as_random the
 * value is the literal; for actual distributions it is @c NaN.
 */
Datum
random_variable_value(PG_FUNCTION_ARGS)
{
  random_variable *rv = (random_variable *) PG_GETARG_POINTER(0);
  PG_RETURN_FLOAT8(rv->val);
}

PG_FUNCTION_INFO_V1(random_variable_make);
/**
 * @brief Build a @c random_variable from a UUID and a cached value.
 *
 * Internal constructor used by the @c provsql.normal /
 * @c provsql.uniform / @c provsql.exponential / @c provsql.as_random
 * SQL constructors after they have created the backing gate.
 */
Datum
random_variable_make(PG_FUNCTION_ARGS)
{
  pg_uuid_t *tok = PG_GETARG_UUID_P(0);
  double val = PG_GETARG_FLOAT8(1);

  random_variable *result = (random_variable *) palloc(sizeof(random_variable));
  memcpy(&result->tok, tok, sizeof(pg_uuid_t));
  result->val = val;
  PG_RETURN_POINTER(result);
}
