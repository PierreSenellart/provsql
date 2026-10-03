/**
 * @file gate_builders.h
 * @brief The gate builders of the query rewriter, for C/C++ callers.
 *
 * The same constructions, at the same addresses, as the SQL-callable
 * @c provenance_times, @c provenance_plus, @c provenance_monus and
 * @c provenance_delta, without the Datum marshalling: rebuilding a circuit
 * with them gives the gates the rewriter would have built from the same
 * operands (see the equation-system resolution of recursive queries).
 */
#ifndef GATE_BUILDERS_H
#define GATE_BUILDERS_H

#include "postgres.h"
#include "utils/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ⊗ of @p n tokens: 𝟙 dropped, none gives 𝟙, one is returned. */
pg_uuid_t provsql_build_times(const pg_uuid_t *tokens, int n);
/** @brief ⊕ of @p n tokens: 𝟘 dropped, none gives 𝟘, one is returned. */
pg_uuid_t provsql_build_plus(const pg_uuid_t *tokens, int n);
/** @brief @p token1 ⊖ @p token2, with X ⊖ X = 𝟘 ⊖ X = 𝟘 and X ⊖ 𝟘 = X. */
pg_uuid_t provsql_build_monus(const pg_uuid_t *token1, const pg_uuid_t *token2);
/** @brief δ(@p token), with δ(𝟘) = 𝟘 and δ(𝟙) = 𝟙. */
pg_uuid_t provsql_build_delta(const pg_uuid_t *token);

#ifdef __cplusplus
}
#endif

#endif /* GATE_BUILDERS_H */
