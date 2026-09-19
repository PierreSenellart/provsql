/**
 * @file agg_token.h
 * @brief Aggregate-provenance token type used in SQL aggregate functions.
 *
 * Defines the @c agg_token composite type that pairs a provenance circuit
 * UUID (as a hex string) with an aggregate running value.  It is passed
 * between the transition and final functions of ProvSQL's custom
 * aggregate operators so that both the provenance token and its
 * associated numeric value travel together through a GROUP BY query.
 */
#ifndef AGG_TOKEN_H
#define AGG_TOKEN_H

#include "provsql_utils.h"

/**
 * @brief Aggregate token bundling a provenance UUID with a running value.
 *
 * @c tok holds the UUID of the provenance circuit gate for the current
 * aggregate group, formatted as a 36-character string
 * (@c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx plus a NUL terminator).
 * The buffer size @c 2*UUID_LEN+5 accommodates the standard UUID text
 * representation.
 *
 * @c val holds the textual representation of the current aggregate value
 * (e.g., a decimal integer or floating-point number).  A longer value (the
 * text of a @c string_agg, an array, a multirange...) keeps its first
 * @c AGG_TOKEN_PREFIX_LEN characters, and the last byte of @c val is set to
 * @c AGG_TOKEN_TRUNCATED: the whole value is that of the gate, read by
 * @c agg_token_value_cstring.
 */
typedef struct agg_token {
  char tok[2*UUID_LEN+5]; ///< Provenance UUID as a text string
  char val[80];           ///< Aggregate running value as a text string
} agg_token;

/** @brief Length of the prefix of a value too long for @c agg_token::val. */
#define AGG_TOKEN_PREFIX_LEN (sizeof(((agg_token *)0)->val) - 2)
/** @brief Last byte of @c agg_token::val when the value is a prefix. */
#define AGG_TOKEN_TRUNCATED '\x01'

void agg_token_set_value(agg_token *aggtok, const char *val, size_t len);
const char *agg_token_value_cstring(const agg_token *aggtok);

#endif /* AGG_TOKEN_H */
