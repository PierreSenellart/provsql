/**
 * @file random_variable_type.c
 * @brief PostgreSQL I/O and constructor functions for the @c random_variable type.
 *
 * @c random_variable is a thin wrapper around a provenance gate UUID,
 * used in user-facing SQL for continuous probabilistic c-tables.
 *
 * On-disk and on-wire text format: the canonical
 * @c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx UUID form (identical to
 * PostgreSQL's @c uuid).  No leading parenthesis / cached scalar / closing
 * parenthesis: every C++ evaluator dispatches on the gate behind the
 * UUID rather than any cached scalar, so the scalar is not load-bearing
 * and only complicated the text I/O.  A binary-coercible
 * @c random_variable -> @c uuid cast (declared @c WITHOUT @c FUNCTION
 * since the two types share their byte layout) keeps SQL ergonomics:
 * an @c rv-typed column flows directly into any function that accepts
 * a @c uuid at zero runtime cost.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "utils/fmgrprotos.h"

/**
 * @brief Binary internal layout of @c random_variable.
 *
 * 16 bytes UUID, no padding, @c TYPALIGN_CHAR (matching @c pg_uuid_t
 * exactly) so a binary-coercible @c CREATE @c CAST is sound -- a
 * cast site reinterprets the same bytes without a runtime
 * conversion function.
 */
typedef struct random_variable {
  pg_uuid_t tok;  ///< Provenance gate UUID
} random_variable;

PG_FUNCTION_INFO_V1(random_variable_in);
/**
 * @brief Parse a @c random_variable from its text representation.
 *
 * Expected format: the standard hyphenated UUID
 * @c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, delegated to @c uuid_in.
 * Raises @c ERROR on malformed input.
 */
Datum
random_variable_in(PG_FUNCTION_ARGS)
{
  char *str = PG_GETARG_CSTRING(0);
  Datum uuid_d = DirectFunctionCall1(uuid_in, CStringGetDatum(str));
  random_variable *result = (random_variable *) palloc(sizeof(random_variable));
  memcpy(&result->tok, DatumGetUUIDP(uuid_d), sizeof(pg_uuid_t));
  PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(random_variable_out);
/**
 * @brief Render a @c random_variable as a hyphenated UUID string.
 */
Datum
random_variable_out(PG_FUNCTION_ARGS)
{
  random_variable *rv = (random_variable *) PG_GETARG_POINTER(0);
  return DirectFunctionCall1(uuid_out, UUIDPGetDatum(&rv->tok));
}

PG_FUNCTION_INFO_V1(random_variable_make);
/**
 * @brief Build a @c random_variable from a UUID.
 *
 * Internal constructor used by the @c provsql.normal /
 * @c provsql.uniform / @c provsql.exponential / @c provsql.as_random
 * SQL constructors after they have created the backing gate.
 * Equivalent in effect to the binary-coercible @c uuid -> @c random_variable
 * cast (both reinterpret the same 16 bytes), but kept as a named
 * function so the constructors read uniformly with the rest of the
 * SQL surface.
 */
Datum
random_variable_make(PG_FUNCTION_ARGS)
{
  pg_uuid_t *tok = PG_GETARG_UUID_P(0);
  random_variable *result = (random_variable *) palloc(sizeof(random_variable));
  memcpy(&result->tok, tok, sizeof(pg_uuid_t));
  PG_RETURN_POINTER(result);
}
