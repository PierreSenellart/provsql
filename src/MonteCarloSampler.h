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

#include <optional>
#include <vector>

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

/**
 * @brief Estimate the joint distribution of @p cmps via Monte Carlo.
 *
 * For each of @p samples worlds, samples the underlying continuous
 * island once (shared @c gate_rv leaves use the same per-iteration
 * draw, per @c monteCarloRV's evalScalar) and evaluates each
 * comparator in @p cmps; the @c k = @p cmps.size() resulting bits
 * form a single word @c w with bit @c i = result of @c cmps[i].  The
 * returned vector has size @c 2^k; entry @c w is the empirical
 * probability that the joint outcome @c w occurred.
 *
 * Used by the multi-cmp half of the hybrid evaluator's island
 * decomposer to inline a categorical distribution over the @c k cmps
 * that share an island; @p cmps must all sit over a continuous
 * island whose scalar evaluation reuses common @c gate_rv leaves so
 * the cmp draws are correctly correlated.
 *
 * @c k is capped at 30 (the result vector size is @c 2^30) to keep
 * memory bounded; the decomposer enforces a much tighter cap
 * (@c k_max in @c HybridEvaluator.cpp) so this is purely a safety
 * limit.  Throws @c CircuitException above the cap.
 *
 * @param gc       The circuit.
 * @param cmps     The comparators jointly evaluated.
 * @param samples  Number of independent worlds.
 * @return         Vector of joint probabilities, indexed by the bit
 *                 word @c w (bit @c i = @c cmps[i] outcome).
 */
std::vector<double> monteCarloJointDistribution(
  const GenericCircuit &gc,
  const std::vector<gate_t> &cmps,
  unsigned samples);

/**
 * @brief Sample a scalar sub-circuit @p samples times and return the draws.
 *
 * @p root must yield a scalar (@c gate_value, @c gate_rv, or @c gate_arith
 * over scalar children); otherwise a @c CircuitException is thrown.  Each
 * iteration uses a fresh per-iteration memo cache so that repeated
 * occurrences of the same @c gate_rv UUID inside an arithmetic expression
 * share their draw within an iteration but not across iterations.
 *
 * The RNG is seeded from @c provsql.monte_carlo_seed exactly like
 * @c monteCarloRV; pinning the GUC makes the returned vector reproducible.
 *
 * Used as the universal MC fallback by the analytical evaluators
 * (@c Expectation, @c HybridEvaluator) when structural shortcuts cannot
 * decide a sub-expression.  Returning the raw draws (rather than a
 * single statistic) lets callers compute any combination of moments
 * from a single sampling pass.
 */
std::vector<double> monteCarloScalarSamples(
  const GenericCircuit &gc, gate_t root, unsigned samples);

/**
 * @brief Outcome of a conditional Monte Carlo sampling pass.
 *
 * @c accepted holds the @c root values from the iterations where
 * @c event_root evaluated to @c true (the rest are rejected).
 * @c attempted is the total number of iterations -- equal to @c samples
 * unless the pass was interrupted -- so the caller can derive the
 * empirical acceptance rate as
 * <tt>accepted.size() / attempted</tt> for diagnostics.
 */
struct ConditionalScalarSamples {
  std::vector<double> accepted;
  unsigned attempted;
};

/**
 * @brief Rejection-sample @p root conditioned on @p event_root.
 *
 * For each of @p samples iterations, the shared @c Sampler resets its
 * per-iteration cache, then:
 *   1. evaluates @p event_root as a Boolean (populating @c bool_cache_
 *      and @c scalar_cache_ for every @c gate_rv / @c gate_input touched);
 *   2. if the indicator is @c true, evaluates @p root as a scalar
 *      using the SAME caches, so any shared @c gate_t leaf produces
 *      one draw that the indicator and the value both observe;
 *   3. otherwise rejects the iteration.
 *
 * This coupling is the entire point of routing the conditional path
 * through one joint circuit: a @c gate_rv reachable from both
 * @p root and @p event_root has the same @c gate_t and therefore
 * shares its per-iteration draw between the indicator (which decides
 * acceptance) and the value (which we record).  The accepted draws
 * are samples from the conditional distribution
 * @f$X \mid A@f$ where @c X = @p root and @c A = @p event_root.
 *
 * @param gc          Circuit (typically from @c getJointCircuit).
 * @param root        Scalar gate whose value we sample.
 * @param event_root  Boolean gate that the iteration must satisfy.
 * @param samples     Number of iterations to attempt.
 */
ConditionalScalarSamples monteCarloConditionalScalarSamples(
  const GenericCircuit &gc, gate_t root, gate_t event_root, unsigned samples);

/**
 * @brief Try to draw @p n exact samples from the conditional
 *        distribution of @p root @b given @p event_root via closed-form
 *        truncation, bypassing MC rejection.
 *
 * Fires only when @p root is a bare @c gate_rv whose family admits a
 * closed-form truncation (@c Uniform / @c Exponential / @c Normal)
 * and @c collectRvConstraints can extract a sound interval from
 * @p event_root.  Other shapes (arith composites, mixtures, Erlang,
 * un-extractable events) return @c std::nullopt so the caller can fall
 * back to @c monteCarloConditionalScalarSamples.
 *
 * Sampling kernels:
 * - <b>Uniform(a, b)</b>: @c collectRvConstraints already intersects
 *   with @c [a, b], so the draw is a plain @c U(lo, hi) on the
 *   intersected interval.  100% acceptance.
 * - <b>Exponential(λ)</b>, one-sided @c X > c: memorylessness yields
 *   @c c + Exp(λ).  Two-sided @c lo < X < hi: inverse-CDF via
 *   @c std::log1p / @c std::expm1 for numerical accuracy near the
 *   support boundary.
 * - <b>Normal(μ, σ)</b>: inverse-CDF transform.  Forward CDF uses
 *   @c std::erf (matching @c AnalyticEvaluator::cdfAt); inverse uses
 *   the Beasley-Springer-Moro rational approximation (~1e-7 accuracy,
 *   ample for sampling).
 *
 * Empty / degenerate truncations (@c lo >= @c hi after intersection)
 * also return @c std::nullopt so the caller's MC fallback can emit
 * its usual "accepted 0" diagnostic.
 *
 * The RNG is seeded from @c provsql.monte_carlo_seed identically to
 * @c monteCarloScalarSamples, so a pinned seed gives reproducible
 * output on either path.
 */
std::optional<std::vector<double>>
try_truncated_closed_form_sample(const GenericCircuit &gc, gate_t root,
                                 gate_t event_root, unsigned n);

}  // namespace provsql

#endif  // PROVSQL_MONTE_CARLO_SAMPLER_H
