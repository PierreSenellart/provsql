/**
 * @file Expectation.h
 * @brief Analytical expectation / variance / moment evaluator over RV circuits.
 *
 * Computes E[X], Var[X], raw moments E[X^k] and central moments
 * E[(X - E[X])^k] over a sub-circuit rooted at a scalar gate
 * (@c gate_value, @c gate_rv, or @c gate_arith), using closed-form
 * formulas wherever the sub-DAG decomposes structurally and falling
 * back to Monte Carlo (via @c monteCarloScalarSamples) when
 * structural shortcuts cannot decide a sub-expression.
 *
 * Decomposition rules:
 * - @c gate_value: literal, constant moments.
 * - @c gate_rv: closed-form moments per @c RandomVariable.h.
 * - @c gate_arith PLUS / MINUS / NEG: linearity of expectation always
 *   applies; variance and higher moments require structural
 *   independence of the children (their reachable @c gate_rv
 *   footprints must be pairwise disjoint).
 * - @c gate_arith TIMES: requires structural independence; otherwise
 *   MC fallback.
 * - @c gate_arith DIV: closed form when the divisor is a deterministic
 *   constant (@c gate_value), otherwise MC fallback.
 *
 * MC fallback is governed by the @c provsql.rv_mc_samples GUC:
 * non-zero values pick the sample count; zero disables the fallback,
 * causing the evaluator to throw rather than sample.  This keeps a
 * single knob shared with future RV-MC fallback paths (Priority 7's
 * island decomposer).
 */
#ifndef PROVSQL_EXPECTATION_H
#define PROVSQL_EXPECTATION_H

#include <optional>

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Compute @f$E[X]@f$ (or @f$E[X \mid A]@f$ if @p event_root is set)
 *        over the scalar sub-circuit rooted at @p root.
 *
 * The conditional path requires that @p event_root be a @c gate_t in
 * the same @c GenericCircuit as @p root -- typically the circuit was
 * loaded via @c getJointCircuit so a shared @c gate_rv between root
 * and event has one @c gate_t, which is what couples the MC sampler.
 * When @p event_root is @c std::nullopt the unconditional path
 * (existing analytical decomposition with MC fallback) is taken
 * unchanged.
 *
 * @throws CircuitException on malformed circuits, unknown
 *         distribution kinds, when @c provsql.rv_mc_samples is 0
 *         and a sub-expression cannot be decomposed analytically,
 *         or when the conditional MC pass accepts too few samples
 *         (suggesting @c P(A) is very small or zero).
 */
double compute_expectation(const GenericCircuit &gc, gate_t root,
                           std::optional<gate_t> event_root = std::nullopt);

/**
 * @brief Compute @f$\mathrm{Var}[X]@f$ (or @f$\mathrm{Var}[X \mid A]@f$
 *        if @p event_root is set) over the scalar sub-circuit rooted
 *        at @p root.  Same exception contract as
 *        @c compute_expectation.
 */
double compute_variance(const GenericCircuit &gc, gate_t root,
                        std::optional<gate_t> event_root = std::nullopt);

/**
 * @brief Compute the raw moment @f$E[X^k]@f$ (or @f$E[X^k \mid A]@f$
 *        if @p event_root is set) for @c k >= 0.
 *
 * @c k = 0 returns 1; @c k = 1 delegates to @c compute_expectation.
 */
double compute_raw_moment(const GenericCircuit &gc, gate_t root, unsigned k,
                          std::optional<gate_t> event_root = std::nullopt);

/**
 * @brief Compute the central moment @f$E[(X - E[X])^k]@f$
 *        (or @f$E[(X - E[X \mid A])^k \mid A]@f$ if @p event_root is set).
 *
 * @c k = 0 returns 1; @c k = 1 returns 0; @c k = 2 returns
 * @c compute_variance.  Higher orders are obtained by binomial
 * expansion in terms of the raw moments returned by
 * @c compute_raw_moment, which inherits the analytical / MC dispatch
 * described above.
 */
double compute_central_moment(const GenericCircuit &gc, gate_t root, unsigned k,
                              std::optional<gate_t> event_root = std::nullopt);

/**
 * @brief Probability that the Boolean subcircuit rooted at @p boolRoot
 *        evaluates to @c true under the tuple-independent
 *        probabilistic-database model.
 *
 * Tries @c BooleanCircuit::independentEvaluation first; if that throws
 * (e.g. the subcircuit is not disconnected for that method), falls
 * back to Monte Carlo with @c provsql.rv_mc_samples samples.  Used by
 * the mixture moment evaluators for compound Boolean Bernoulli wires.
 */
double evaluateBooleanProbability(const GenericCircuit &gc, gate_t boolRoot);

}  // namespace provsql

#endif  // PROVSQL_EXPECTATION_H
