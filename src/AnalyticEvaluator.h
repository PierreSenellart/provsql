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
#include "RandomVariable.h"  // DistributionSpec

namespace provsql {

/**
 * @brief Closed-form CDF @f$F_X(c) = P(X \le c)@f$ for a basic
 *        continuous distribution.
 *
 * Returns the cumulative distribution at @p c for the distribution
 * @p d.  Used internally by @c AnalyticEvaluator's @c gate_cmp
 * resolution and by the @c HybridEvaluator decomposer's
 * monotone-shared-scalar fast path to compute interval probabilities
 * analytically (no MC noise) when the shared scalar is a bare
 * @c gate_rv.  Returns @c NaN when @p d carries a parameter shape
 * the CDF doesn't cover (e.g. non-integer Erlang shape, which would
 * require the regularised lower incomplete gamma function).
 *
 * - Normal(μ, σ):   @f$\Phi((c - \mu) / \sigma)@f$ via @c std::erf.
 * - Uniform(a, b):  piecewise linear; 0 for @c c<=a, 1 for
 *                   @c c>=b, @c (c - a) / (b - a) otherwise.
 * - Exponential(λ): @c 1 - exp(-λc) for @c c>0; 0 for @c c<=0.
 * - Erlang(k, λ) (integer @c k≥1): finite-sum form
 *                   @f$1 - e^{-\lambda c} \sum_{n=0}^{k-1}
 *                   (\lambda c)^n / n!@f$ for @c c>0.
 */
double cdfAt(const DistributionSpec &d, double c);

/**
 * @brief Closed-form probability density @f$f(c)@f$ for a basic
 *        distribution.
 *
 * Used by @c rv_analytical_curves to ship a sampled curve to clients
 * (Studio's Distribution profile overlay).  Returns @c 0 outside the
 * natural support and @c NaN for parameter shapes the analytical
 * form doesn't cover (e.g. non-integer Erlang shape).
 *
 * - Normal(μ, σ):   @f$\frac{1}{\sigma\sqrt{2\pi}}
 *                       \exp(-(c-\mu)^2 / (2\sigma^2))@f$.
 * - Uniform(a, b):  @c 1/(b-a) for @c a<=c<=b, @c 0 otherwise.
 * - Exponential(λ): @c λ·exp(-λc) for @c c>=0, @c 0 otherwise.
 * - Erlang(k, λ) (integer @c k>=1):
 *                   @f$\frac{\lambda^k c^{k-1} e^{-\lambda c}}{(k-1)!}@f$
 *                   for @c c>=0, @c 0 otherwise.
 */
double pdfAt(const DistributionSpec &d, double c);

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
