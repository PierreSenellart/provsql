/**
 * @file AnalyticEvaluator.h
 * @brief Closed-form CDF resolution for trivial @c gate_cmp shapes.
 *
 * For comparators that reduce to either:
 * - @c X @c cmp @c c with @c X a bare @c gate_rv leaf and @c c a
 *   bare @c gate_value constant, or
 * - @c X @c cmp @c Y with @c X and @c Y two distinct independent
 *   normal @c gate_rv leaves,
 *
 * the comparator's probability has a closed form via the
 * distribution's CDF.  Implementations live inline in the @c .cpp
 * (normal: @c std::erf; uniform: arithmetic; exponential:
 * @c std::expm1) so the pass has no external math dependency.  The
 * difference of two independent normals is itself normal, with the
 * obvious mean and variance.  Replacing the @c gate_cmp by a
 * Bernoulli @c gate_input carrying the analytical probability lets
 * the surrounding circuit be evaluated exactly by every downstream
 * probability method &ndash; no MC noise.
 *
 * Unlike @c RangeCheck, this pass is probability-specific:
 * fractional probabilities are meaningful only on the probability
 * path.  It therefore runs in @c probability_evaluate.cpp, not
 * inside @c getGenericCircuit (which is shared with semiring
 * evaluation, view_circuit, PROV export, etc.).
 *
 * Compositions involving @c gate_arith (e.g. <tt>aX + bY > c</tt>
 * for independent normals) are out of scope here; folding them
 * into a single distribution requires the family-closure simplifier
 * planned for the hybrid evaluator.
 */
#ifndef PROVSQL_ANALYTIC_EVALUATOR_H
#define PROVSQL_ANALYTIC_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the closed-form CDF resolution pass over @p gc.
 *
 * For every @c gate_cmp in the circuit whose two sides match one of
 * the supported shapes (see the header docstring), computes the
 * comparator's probability analytically and replaces the cmp by a
 * Bernoulli @c gate_input via @c GenericCircuit::resolveCmpToBernoulli.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runAnalyticEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_ANALYTIC_EVALUATOR_H
