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
 * - @c gate_input (and @c gate_update) – Bernoulli draw at @c getProb,
 *   memoised per iteration (so the same input feeding two children
 *   produces the same draw).
 * - @c gate_plus / @c gate_times / @c gate_monus – Boolean OR / AND /
 *   AND-NOT.
 * - @c gate_zero / @c gate_one – false / true.
 * - @c gate_cmp with scalar (@c gate_rv / @c gate_arith / @c gate_value)
 *   children – compare two scalar samples per the comparison-operator
 *   OID stored in @c info1.  Aggregate-vs-constant @c gate_cmp gates
 *   from HAVING semantics are handled by the existing
 *   @c BooleanCircuit path and are not reached here.
 * - @c gate_value – parse @c extra as @c float8.
 * - @c gate_rv – fresh draw from the distribution serialised in
 *   @c extra (memoised per iteration so the SAME RV inside an
 *   arithmetic expression uses the same draw, per the thesis's
 *   SampleOne).
 * - @c gate_arith – recurse on scalar children, combine per the
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
#include <utility>
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
 * @brief Whole-circuit @c (eps,delta)-relative probability via the
 *        Dagum-Karp-Luby-Ross stopping rule.
 *
 * The general-Bernoulli case of @c BooleanCircuit::karpLubyStopping, driven by
 * the RV-aware @c Sampler's @c evalBool rather than by DNF coverage trials, so
 * it applies to ANY circuit the sampler can evaluate (plain Boolean, continuous
 * @c gate_rv, and HAVING @c gate_cmp / @c gate_agg) -- the universal relative
 * estimator.  Draws whole-circuit worlds until the success count reaches the
 * threshold @c Y1 = 1 + (1+eps)*4*(e-2)*ln(2/delta)/eps^2, then returns
 * @c Y1/N: a relative @c (eps,delta) approximation of @c Pr[root].  The sample
 * count @c N adapts to the true @c Pr[root] (expected @c Y1/Pr[root]), so the
 * cost is polynomial precisely when @c Pr[root] is at least @c 1/poly.
 *
 * Sampling stops early at @p max_samples worlds; @p reached_target is then
 * @c false and the return is the plain unbiased @c success/N mean over the
 * spent budget (the relative target was not met -- the caller reports the
 * weaker, additive guarantee actually achieved).
 *
 * @param gc              The circuit.
 * @param root            Gate to evaluate as a Boolean event.
 * @param eps             Target relative error (in @c (0,1]).
 * @param delta           Target failure probability (in @c (0,1)).
 * @param max_samples     Hard cap on the number of worlds drawn.
 * @param samples_used    Output: worlds actually drawn.
 * @param reached_target  Output: whether the threshold was reached before the
 *                        cap (i.e. the relative guarantee holds).
 * @return                The probability estimate.
 */
double monteCarloRVStopping(const GenericCircuit &gc, gate_t root,
                            double eps, double delta,
                            unsigned long max_samples,
                            unsigned long &samples_used,
                            bool &reached_target);

/**
 * @brief Walk the circuit reachable from @p root looking for any @c gate_rv.
 *
 * Used by @c probability_evaluate to dispatch between the existing
 * @c BooleanCircuit path and the RV-aware sampler in this file.
 */
bool circuitHasRV(const GenericCircuit &gc, gate_t root);

/**
 * @brief Whether a surviving @c gate_agg exists and every one is sample-faithful
 *        (@c SUM / @c AVG / @c MIN / @c MAX / @c COUNT -- every aggregate the
 *        sampler reproduces exactly).
 *
 * A @c gate_agg the exact closed-form / marginal-vector pre-passes did not fold
 * into a Bernoulli @c gate_input marks a HAVING aggregate comparator whose exact
 * resolution needs @c provsql_having's threshold-lineage expansion -- which does
 * not terminate in practice for a large-magnitude / large-support aggregate
 * (the dense @c kMaxSumRange and sparse @c kMaxSumSupport caps exceeded).  For
 * an @c (eps,delta) request @c probability_evaluate uses this to route the
 * circuit straight to the world-sampler (the @c gate_agg arm of @c evalScalar)
 * -- a sound FPRAS for the apx-safe corner of the HAVING trichotomy -- instead
 * of attempting the non-terminating Boolean expansion.
 *
 * The sampler's @c gate_agg arm pushes each kept contributor's value into the
 * matching @c Aggregator, reproducing SQL semantics exactly: the value gate is
 * the row's contribution (the summed term for @c SUM; the 0/1 indicator for
 * @c COUNT, 0 for a NULL row so @c count(x) does not count NULLs; the compared
 * value for @c AVG / @c MIN / @c MAX), so NULL rows are handled and an empty
 * group finalises to the value the exact evaluator uses (0 for @c SUM / @c COUNT,
 * NaN -> comparison false for the others), and @c gate_arith over them is covered
 * too.  In practice only @c SUM / @c AVG / @c MIN / @c MAX ever reach here:
 * @c COUNT's value-support is small (0/1 per row) so it is always resolved
 * exactly and never bails -- but it is sample-faithful as well, so it is not
 * excluded.
 */
bool circuitHasUnresolvedSampleableAgg(const GenericCircuit &gc, gate_t root);

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
 * @brief Coupled per-iteration draws of two scalar roots.
 *
 * Each iteration resets the per-iteration cache once and evaluates both
 * roots against it, so any stochastic leaf shared between @p root_a and
 * @p root_b produces a single draw both observe: the returned pairs are
 * samples from the JOINT distribution of (A, B).  Backs the
 * mutual-information plug-in estimator.
 */
std::pair<std::vector<double>, std::vector<double>>
monteCarloScalarPairSamples(const GenericCircuit &gc, gate_t root_a,
                            gate_t root_b, unsigned samples);

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

/**
 * @brief Outcome of a likelihood-weighting (importance-sampling) pass.
 *
 * Latent-variable posterior inference draws latents from the prior via the
 * forward recursion and weights each draw by the observed leaves' densities
 * at the data (self-normalised importance sampling; the continuous
 * generalisation of rejection conditioning, which is the 0/1-weight case).
 *
 * @c particles holds one @c (x, w) pair per prior draw with @b positive
 * weight (@c x = the queried root's value, @c w = the product of the
 * evidence factors); the caller derives any weighted posterior statistic
 * (mean, variance, quantile) from them.  @c weight_sum / @c weight_sq_sum
 * accumulate over @b all @c attempted draws (a zero-weight draw contributes
 * 0), so @c evidence() is the marginal likelihood @c P(data) and
 * @c effectiveSampleSize() the ESS diagnostic.
 */
struct WeightedPosterior {
  std::vector<std::pair<double, double>> particles;  ///< (x, w) with w > 0.
  double weight_sum = 0.0;      ///< Sum of w over all attempted draws.
  double weight_sq_sum = 0.0;   ///< Sum of w^2 over all attempted draws.
  unsigned attempted = 0;       ///< Number of prior draws.

  /// Marginal likelihood P(data): the mean raw importance weight.
  double evidence() const {
    return attempted ? weight_sum / static_cast<double>(attempted) : 0.0;
  }
  /// Effective sample size (Sum w)^2 / (Sum w^2); 0 when all weights are 0.
  double effectiveSampleSize() const {
    return weight_sq_sum > 0.0 ? (weight_sum * weight_sum) / weight_sq_sum : 0.0;
  }
};

/**
 * @brief Self-normalised importance sampling of @p root given @p evidence.
 *
 * For each of @p samples prior draws the shared @c Sampler resets its
 * per-iteration caches, then:
 *   1. evaluates @p evidence to an importance @b weight (@c evalWeight):
 *      a @c gate_observe contributes its leaf's pdf at the datum, a Boolean
 *      conditioning event contributes a 0/1 weight, a @c gate_times
 *      multiplies its children's weights -- populating @c scalar_cache_ for
 *      every latent the evidence touches;
 *   2. if the weight is positive, evaluates @p root as a scalar using the
 *      SAME caches, so a latent shared between @p root and @p evidence is
 *      drawn once and the weight and the value observe it jointly;
 *   3. records the @c (value, weight) particle.
 *
 * Coupling the weight and the value through one joint circuit
 * (@c getJointCircuit) is what makes the shared latent a single @c gate_t;
 * the particles are then draws from the posterior of @c root given the data.
 *
 * @param gc        Circuit (typically from @c getJointCircuit).
 * @param root      Scalar gate whose posterior we sample.
 * @param evidence  Evidence circuit (an @c and_agg conjunction of
 *                  @c gate_observe / Boolean events).
 * @param samples   Number of prior draws.
 */
WeightedPosterior importanceSampleConditional(
  const GenericCircuit &gc, gate_t root, gate_t evidence, unsigned samples);

/**
 * @brief Marginal likelihood @c P(data) of @p evidence: the mean raw
 *        importance weight over @p samples prior draws.
 *
 * The same quantity rejection conditioning computes as @c P(C), now a
 * product of the observations' densities.  Backs @c provsql.evidence.
 */
double importanceEvidence(const GenericCircuit &gc, gate_t evidence,
                          unsigned samples);

/**
 * @brief Sampling-importance-resampling: draw @p n posterior samples from a
 *        weighted particle set (proportional to weight, with replacement).
 *
 * Turns the weighted particles of @c importanceSampleConditional into
 * (approximately) unweighted posterior draws for @c rv_sample.  Returns an
 * empty vector when there is no positive-weight particle.  The RNG is
 * seeded from @c provsql.monte_carlo_seed, like every other sampling path.
 */
std::vector<double> posteriorResample(const WeightedPosterior &post,
                                      unsigned n);

/**
 * @brief Whether the circuit reachable from @p root contains a
 *        @c gate_observe -- the signal that a conditioning event is
 *        continuous-density evidence and must be evaluated by importance
 *        sampling rather than the analytic / rejection conditional paths.
 */
bool circuitHasObserve(const GenericCircuit &gc, gate_t root);

}  // namespace provsql

#endif  // PROVSQL_MONTE_CARLO_SAMPLER_H
