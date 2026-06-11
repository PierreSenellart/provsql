/**
 * @file mobius_evaluate.h
 * @brief Shared declaration for the Möbius-route probability sweep.
 *
 * The safe-UCQ Möbius compiler (@c mobius_evaluate.cpp) materialises a circuit
 * rooted at a @c gate_mobius signed combination over certified-independent
 * Boolean islands.  Its probability is a single linear sweep that, at each
 * @c gate_mobius, sums the children's probabilities with the stored integer
 * coefficients (clamped to [0,1]); the Boolean islands evaluate read-once.  The
 * sweep lives next to the rest of the probability machinery
 * (@c probability_evaluate.cpp) but is called both there (the @c gate_mobius
 * root dispatch) and from the stats SRF, hence this shared declaration.
 */
#ifndef MOBIUS_EVALUATE_H
#define MOBIUS_EVALUATE_H

extern "C" {
#include "postgres.h"
#include "utils/uuid.h"
}

/**
 * @brief Probability of a Möbius-route token (a @c gate_mobius-rooted circuit,
 *        or any Boolean island beneath one).  Linear sweep: independent OR /
 *        AND on the Boolean islands, signed coefficient combination at each
 *        @c gate_mobius, clamped to [0,1].
 */
double mobius_probability_of(pg_uuid_t token);

#endif /* MOBIUS_EVALUATE_H */
