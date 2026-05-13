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

#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "GenericCircuit.h"
#include "RandomVariable.h"

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
 *
 * When @p event_root is set, the returned interval is the
 * intersection of the unconditional support with the per-RV
 * constraints implied by the AND-conjunct chain rooted at the event
 * (`rv op c` cmps over @p root collected via the same walker
 * @c runRangeCheck uses for joint feasibility).  Constraints we
 * cannot interpret are silently skipped: the result is then a
 * conservative superset of the true conditional support, never a
 * subset.
 */
std::pair<double, double>
compute_support(const GenericCircuit &gc, gate_t root,
                std::optional<gate_t> event_root = std::nullopt);

/**
 * @brief Walk @p event_root collecting `rv op c` constraints on @p target_rv.
 *
 * Descends through AND-conjunct factors (@c gate_times, @c gate_one,
 * Boolean leaves whose footprint doesn't include @p target_rv -- these
 * are independent of the RV and contribute no truncation) collecting
 * every @c gate_cmp interpretable as `target_rv op c` for a constant
 * @c c, and intersects them into a running interval seeded with the
 * unconditional support of @p target_rv.
 *
 * Returns the resulting interval as @c (lo, hi), or @c std::nullopt
 * if the walk found a structure that defeats the recognisers (a
 * @c gate_plus / @c gate_monus disjunction over the chain, a cmp
 * shape other than `rv op const`, ...).  Callers treat
 * @c std::nullopt as "fall back to the unconditional case" --
 * sound for support and MC fallback for moments.
 *
 * Constraints on RVs other than @p target_rv are ignored; they affect
 * @c P(A) but not the truncation of the target's distribution.
 */
std::optional<std::pair<double, double>>
collectRvConstraints(const GenericCircuit &gc, gate_t event_root,
                     gate_t target_rv);

/**
 * @brief Detection result for a closed-form, optionally-truncated
 *        single-RV shape.
 *
 * Carries the parsed distribution and the @c [lo, hi] support after
 * intersection with the conditioning event (or the natural support
 * when @c truncated == @c false).  Either bound may be infinite when
 * the RV's natural support or the event leaves the corresponding
 * side unbounded (e.g. @c Normal | @c X > 0 yields
 * @c (0, +&infin;)).
 */
struct TruncatedSingleRv {
  DistributionSpec spec;  ///< Parsed kind + parameters
  double lo;              ///< Lower bound (-INF if unbounded)
  double hi;              ///< Upper bound (+INF if unbounded)
  bool truncated;         ///< True iff the bounds came from a
                          ///< non-trivial @c event_root
};

/**
 * @brief Detect a closed-form, optionally-truncated single-RV shape.
 *
 * Common shape-detection helper shared by every closed-form
 * single-RV consumer:
 * - @c try_truncated_closed_form (truncated moments,
 *   @c Expectation.cpp);
 * - @c try_truncated_closed_form_sample (rejection-free sampling,
 *   @c MonteCarloSampler.cpp);
 * - @c rv_analytical_curves (PDF / CDF overlay,
 *   @c RvAnalyticalCurves.cpp).
 *
 * Returns @c std::nullopt when the shape is not tractable:
 *  - @p root is not a bare @c gate_rv;
 *  - the gate's @c extra does not parse as a @c DistributionSpec;
 *  - @p event_root resolves to @c gate_zero (event already decided
 *    infeasible by @c runRangeCheck);
 *  - @c collectRvConstraints fails (incomplete walk);
 *  - the resulting interval is empty or degenerate
 *    (@c lo @>= @c hi).
 *
 * When @p event_root is omitted or resolves to @c gate_one, the
 * returned @c TruncatedSingleRv carries the RV's natural support and
 * @c truncated = @c false; callers that don't distinguish the
 * conditional and unconditional cases (e.g. the analytical-curves
 * x-range chooser) can read uniformly off the result.
 */
std::optional<TruncatedSingleRv>
matchTruncatedSingleRv(const GenericCircuit &gc, gate_t root,
                       std::optional<gate_t> event_root);

/**
 * @brief True iff the conditioning event is provably infeasible for
 *        a bare @c gate_rv root.
 *
 * Distinguishes "event proved infeasible" (event resolves to
 * @c gate_zero, or @c collectRvConstraints intersects to an empty
 * interval) from "shape unsupported by @c matchTruncatedSingleRv"
 * (return @c std::nullopt that just means "fall back to MC").
 *
 * Used by the conditional-moment dispatcher to raise an explicit
 * infeasibility error before falling through to MC rejection — MC
 * would still detect the same condition by accepting 0 of N samples,
 * but the closed-form predicate spots it without ten thousand
 * wasted draws and emits a tighter message.
 *
 * Returns @c false for non-@c gate_rv roots and for roots whose
 * event/support pair is not provably infeasible by this cheap pass
 * (the caller can still proceed to MC).
 */
bool eventIsProvablyInfeasible(const GenericCircuit &gc, gate_t root,
                               std::optional<gate_t> event_root);

/**
 * @brief Point mass at a finite scalar value (a @c gate_value root, or
 *        an @c as_random(c) leaf surfaced as a @c gate_value).
 *
 * Unconditional only; @c matchClosedFormDistribution bails when an
 * event_root is supplied for a Dirac root.  Carries no probability
 * field: a Dirac root standalone has total mass 1, and inside a
 * @c BernoulliMixtureShape the parent's @c p / 1-p weight is applied
 * by the curve renderer.
 */
struct DiracShape {
  double value;
};

/**
 * @brief Categorical distribution over a finite outcome set.
 *
 * Matches a categorical-form @c gate_mixture
 * (@c isCategoricalMixture): the key @c gate_input is ignored (its
 * own probability is irrelevant — the mass is on the @c mulinputs),
 * each remaining wire contributes one @c {value, prob} pair.
 *
 * Unconditional only; conditioning on a categorical's value is
 * handled upstream by @c RangeCheck folding the cmp into a Bernoulli,
 * leaving no event_root for this matcher to see.
 */
struct CategoricalShape {
  std::vector<std::pair<double, double>> outcomes;  ///< (value, mass) pairs
};

struct BernoulliMixtureShape;  // forward (variant cycle)

/**
 * @brief One of the closed-form shapes the analytical-curves payload
 *        can render: bare RV (continuous PDF/CDF), Dirac (point mass),
 *        categorical (multiple point masses), or Bernoulli mixture
 *        of any two of the above.
 */
using ClosedFormShape = std::variant<TruncatedSingleRv,
                                     DiracShape,
                                     CategoricalShape,
                                     BernoulliMixtureShape>;

/**
 * @brief Bernoulli mixture (@c gate_mixture with the
 *        @c [p_token, x_token, y_token] shape).
 *
 * @c p is @c getProb(p_token) when @c p_token is a bare
 * @c gate_input; a compound Boolean @c p_token bails (its probability
 * would require a recursive @c probability_evaluate call, out of
 * scope for the static predicate).  @c left and @c right recursively
 * match the two arms; either may itself be a mixture, a bare RV, a
 * Dirac, or a categorical — but always unconditional (truncation
 * under a mixture is deferred until a real query needs it; see
 * @c TODO2.md / Item 6a).
 */
struct BernoulliMixtureShape {
  double p;
  std::shared_ptr<ClosedFormShape> left;
  std::shared_ptr<ClosedFormShape> right;
};

/**
 * @brief Detect any of the closed-form shapes supported by
 *        @c rv_analytical_curves.
 *
 * Generalisation of @c matchTruncatedSingleRv that adds Bernoulli
 * mixtures, categoricals, and Dirac (scalar @c gate_value) roots.
 * Conditioning (@p event_root) is honoured for bare RV roots only;
 * Dirac / categorical / mixture roots bail when the event isn't
 * @c gate_one (the post-load-simplification "always true" default).
 *
 * Returns @c std::nullopt when none of the supported shapes match;
 * callers fall back to histogram-only rendering.
 */
std::optional<ClosedFormShape>
matchClosedFormDistribution(const GenericCircuit &gc, gate_t root,
                            std::optional<gate_t> event_root);

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
