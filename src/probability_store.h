/**
 * @file probability_store.h
 * @brief Write-once probabilities: the entry points other files use.
 *
 * See @c probability_store.c for what "written once" means and how a
 * rolled-back write is undone.
 */
#ifndef PROVSQL_PROBABILITY_STORE_H
#define PROVSQL_PROBABILITY_STORE_H

#include "postgres.h"
#include "provsql_utils.h"

/**
 * @brief Write @p token's probability and record the write, so that a
 *        rollback of the current (sub)transaction clears it.
 *
 * Raises when the gate carries no probability, when @p prob is outside
 * [0, 1], and when the gate already holds a different probability.
 * Writing the value the gate already holds is a no-op.
 */
void provsql_set_prob_tracked(const pg_uuid_t *token, double prob);

#endif /* PROVSQL_PROBABILITY_STORE_H */
