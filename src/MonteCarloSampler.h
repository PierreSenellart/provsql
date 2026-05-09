/**
 * @file MonteCarloSampler.h
 * @brief Monte Carlo sampling over a @c GenericCircuit, RV-aware.
 *
 * Drop-in replacement for @c BooleanCircuit::monteCarlo for circuits
 * that contain continuous random variables (@c gate_rv) or arithmetic
 * over RVs (@c gate_arith).  Operates directly on the
 * @c GenericCircuit produced by @c CircuitFromMMap, so the
 * BoolExpr-semiring translation that drops non-Boolean gates is not
 * needed.
 *
 * Gate handling:
 * - @c gate_input (and @c gate_update) — Bernoulli draw at @c getProb,
 *   memoised per iteration (so the same input feeding two children
 *   produces the same draw).
 * - @c gate_plus / @c gate_times / @c gate_monus — Boolean OR / AND /
 *   AND-NOT.
 * - @c gate_zero / @c gate_one — false / true.
 * - @c gate_cmp with scalar (@c gate_rv / @c gate_arith / @c gate_value)
 *   children — compare two scalar samples per the comparison-operator
 *   OID stored in @c info1.  Aggregate-vs-constant @c gate_cmp gates
 *   from HAVING semantics are handled by the existing
 *   @c BooleanCircuit path and are not reached here.
 * - @c gate_value — parse @c extra as @c float8.
 * - @c gate_rv — fresh draw from the distribution serialised in
 *   @c extra (memoised per iteration so the SAME RV inside an
 *   arithmetic expression uses the same draw, per the thesis's
 *   SampleOne).
 * - @c gate_arith — recurse on scalar children, combine per the
 *   operator tag in @c info1 (@c provsql_arith_op enum: PLUS / TIMES
 *   are n-ary; MINUS / DIV are binary; NEG is unary).
 *
 * The RNG is seeded from the @c provsql.monte_carlo_seed GUC: zero
 * (default) seeds non-deterministically from @c std::random_device,
 * any other value is a literal seed shared across the Bernoulli and
 * continuous paths so a single GUC pins the whole computation.
 */
#ifndef PROVSQL_MONTE_CARLO_SAMPLER_H
#define PROVSQL_MONTE_CARLO_SAMPLER_H

#include "GenericCircuit.h"

extern "C" {
#include "provsql_utils.h"
}

namespace provsql {

/**
 * @brief Run Monte Carlo on a circuit that may contain @c gate_rv leaves.
 *
 * @param gc       The circuit (loaded from the mmap store via
 *                 @c CircuitFromMMap).
 * @param root     Gate to evaluate as a Boolean expression.
 * @param samples  Number of independent worlds to sample.
 * @return         Estimated probability that @p root is true.
 *
 * @throws CircuitException on malformed circuits (unknown gate kind in
 *         a Boolean position, malformed @c extra, unknown comparison
 *         operator, etc.).
 */
double monteCarloRV(const GenericCircuit &gc, gate_t root, unsigned samples);

/**
 * @brief Walk the circuit reachable from @p root looking for any @c gate_rv.
 *
 * Used by @c probability_evaluate to dispatch between the existing
 * @c BooleanCircuit path and the RV-aware sampler in this file.
 */
bool circuitHasRV(const GenericCircuit &gc, gate_t root);

}  // namespace provsql

#endif  // PROVSQL_MONTE_CARLO_SAMPLER_H
