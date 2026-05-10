/**
 * @file RangeCheck.h
 * @brief Support-based bound check for continuous-RV comparators.
 *
 * Each continuous distribution has a known support: @c uniform(a,b)
 * on @c [a,b], @c exponential(λ) on @c [0,∞), @c normal on @c ℝ.
 * Interval arithmetic propagates these supports through @c gate_arith
 * combinators, giving a per-gate @c [lo,hi] bound on the value the
 * gate can take in any possible world.  When a @c gate_cmp's two
 * sides have provably-disjoint supports, the comparator's probability
 * is 0 or 1 exactly &ndash; no sampling required.
 *
 * The peephole pass walks every @c gate_cmp reachable from a root,
 * decides probabilities where it can, and replaces decided
 * comparators by Bernoulli @c gate_input gates via
 * @c GenericCircuit::resolveCmpToBernoulli.  Anything it cannot
 * decide is left untouched for downstream evaluators (analytic CDFs
 * where they apply, Monte Carlo otherwise).
 *
 * No external dependency.
 */
#ifndef PROVSQL_RANGE_CHECK_H
#define PROVSQL_RANGE_CHECK_H

#include <utility>

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Compute the @c [lo, hi] support interval of a scalar
 *        sub-circuit rooted at @p root.
 *
 * Same interval-arithmetic propagation @c runRangeCheck uses
 * internally, exposed for the SQL @c support() function:
 * - @c gate_value: point @c [c, c].
 * - @c gate_rv:    distribution support (uniform exact, exponential
 *                  on @c [0, +∞), normal on @c (-∞, +∞)).
 * - @c gate_arith: propagated through @c +, @c −, @c ×, @c /, unary
 *                  @c −.
 *
 * Anything else collapses to the conservative all-real interval
 * @c (-∞, +∞).  Never throws on unrecognised gates -- callers receive
 * the wide interval instead, which is the right semantic for "we
 * cannot prove a tighter bound".
 */
std::pair<double, double>
compute_support(const GenericCircuit &gc, gate_t root);

/**
 * @brief Run the support-based pruning pass over @p gc.
 *
 * For every @c gate_cmp in the circuit, computes the interval of
 * @c (lhs - rhs) via interval arithmetic over @c gate_value,
 * @c gate_rv, and @c gate_arith leaves; when the interval is
 * provably above, below, or disjoint from zero, replaces the
 * @c gate_cmp by a Bernoulli @c gate_input carrying the decided
 * probability (0 or 1).
 *
 * Comparators whose interval is inconclusive (overlaps zero) are
 * left intact for downstream passes.
 *
 * Iterates every gate (rather than walking from a specific root) so
 * that a single sweep at @c getGenericCircuit time benefits every
 * downstream consumer regardless of which sub-circuit they later
 * traverse.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runRangeCheck(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_RANGE_CHECK_H
