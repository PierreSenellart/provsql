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

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Compute @f$E[X]@f$ over the scalar sub-circuit rooted at @p root.
 *
 * @throws CircuitException on malformed circuits, unknown
 *         distribution kinds, or when @c provsql.rv_mc_samples is 0
 *         and a sub-expression cannot be decomposed analytically.
 */
double compute_expectation(const GenericCircuit &gc, gate_t root);

/**
 * @brief Compute @f$\mathrm{Var}[X]@f$ over the scalar sub-circuit
 *        rooted at @p root.  Same exception contract as
 *        @c compute_expectation.
 */
double compute_variance(const GenericCircuit &gc, gate_t root);

/**
 * @brief Compute the raw moment @f$E[X^k]@f$ for @c k >= 0.
 *
 * @c k = 0 returns 1; @c k = 1 delegates to @c compute_expectation.
 */
double compute_raw_moment(const GenericCircuit &gc, gate_t root, unsigned k);

/**
 * @brief Compute the central moment @f$E[(X - E[X])^k]@f$ for @c k >= 0.
 *
 * @c k = 0 returns 1; @c k = 1 returns 0; @c k = 2 returns
 * @c compute_variance.  Higher orders are obtained by binomial
 * expansion in terms of the raw moments returned by
 * @c compute_raw_moment, which inherits the analytical / MC dispatch
 * described above.
 */
double compute_central_moment(const GenericCircuit &gc, gate_t root, unsigned k);

}  // namespace provsql

#endif  // PROVSQL_EXPECTATION_H
