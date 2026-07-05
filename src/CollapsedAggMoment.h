/**
 * @file CollapsedAggMoment.h
 * @brief Rao-Blackwellised (collapsed) evaluation of a correlated COUNT / SUM
 *        and of a latent conditioned on such a count.
 *
 * The recurring latent-variable relational shape is an aggregate over
 * probabilistically-selected rows whose selection events are coupled by ONE
 * shared continuous latent.  Conditional on that latent the row indicators are
 * independent, so the aggregate's moments -- and the posterior of a latent
 * conditioned on the aggregate -- collapse to a 1-D quadrature over the shared
 * latent instead of an @c n^k tuple enumeration or a degenerating importance
 * sampler.  See @c CollapsedAggMoment.cpp for the recognised shape.
 */
#ifndef PROVSQL_COLLAPSED_AGG_MOMENT_H
#define PROVSQL_COLLAPSED_AGG_MOMENT_H

#include <optional>

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Collapsed raw moment @c E[C^k] of a correlated COUNT / SUM @p agg,
 *        or @c std::nullopt when the circuit does not match the shared-latent
 *        shape.  @c k in @c {1, 2}.
 */
std::optional<double>
aggCollapsedRawMoment(const GenericCircuit &gc, gate_t agg, unsigned k);

/**
 * @brief Collapsed exact posterior raw moment @c E[R^k | Y = C] for a latent
 *        @p target R conditioned (through the equality event @p event) on a
 *        discrete rv @c Y -- parametrised by @p target -- equalling a
 *        correlated COUNT @c C.
 *
 * @p event must be a @c gate_cmp with the @c = operator whose operands are a
 * parametric discrete @c gate_rv over @p target and a @c gate_agg count.  The
 * count's pmf @c P(C=j) is obtained by the collapse; the posterior is then the
 * 1-D quadrature
 * @f$E[R^k|C] = \int r^k f_R(r) L(r)\,dr / \int f_R(r) L(r)\,dr@f$ with
 * likelihood @f$L(r) = \sum_j P(C=j)\,\mathrm{pmf}_Y(j;\theta(r))@f$ -- exact
 * up to quadrature, @c O(n^2 + G K).  @c std::nullopt on any shape mismatch
 * (the caller then falls back to importance sampling).  @c k in @c {1, 2}.
 */
std::optional<double>
collapsedConditionalMoment(const GenericCircuit &gc, gate_t target,
                           gate_t event, unsigned k);

}  // namespace provsql

#endif  // PROVSQL_COLLAPSED_AGG_MOMENT_H
