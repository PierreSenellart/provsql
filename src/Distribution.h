/**
 * @file Distribution.h
 * @brief Per-family polymorphic view over a continuous @c gate_rv
 *        distribution (§F.1 class hierarchy).
 *
 * Replaces the @c DistKind @c switch blocks scattered across the RV
 * evaluators with one virtual dispatch per family.  A @c Distribution is a
 * transient view constructed from a parsed @c DistributionSpec via
 * @ref makeDistribution -- there is no gate-ABI or on-disk change; the
 * @c extra text encoding and @c DistributionSpec POD are unchanged.
 *
 * Adding a family becomes a new subclass + a factory arm (the migration is
 * ongoing; see @c doc/TODO/distribution-refactor.md).  Methods that a
 * family cannot answer follow the existing NaN-as-undecided contract so the
 * analytic paths fall back to Monte Carlo unchanged.
 */
#ifndef PROVSQL_DISTRIBUTION_H
#define PROVSQL_DISTRIBUTION_H

#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "RandomVariable.h"  // DistKind, DistributionSpec

namespace provsql {

/** @brief A closed support interval [lo, hi] (±infinity for unbounded). */
struct DistSupport {
  double lo;
  double hi;
};

/**
 * @brief Abstract per-family continuous distribution.
 *
 * Concrete subclasses (Normal / Uniform / Exponential / Erlang) hold the
 * two parameters and implement the family-specific closed forms.  The
 * interface grows as consumers migrate off their @c DistKind switches; the
 * methods below are the family-local ones (pairwise closure / comparison
 * rules live in separate registries, not here).
 */
class Distribution {
public:
  virtual ~Distribution() = default;

  /** @name Identity (parameters, for the i.i.d. equality test) */
  ///@{
  virtual DistKind kind() const = 0;
  virtual double p1() const = 0;
  virtual double p2() const = 0;
  ///@}

  /** @name Closed-form moments */
  ///@{
  virtual double mean() const = 0;                 ///< E[X]
  virtual double variance() const = 0;             ///< Var(X)
  virtual double rawMoment(unsigned k) const = 0;  ///< E[X^k]
  ///@}

  /** @name Density / distribution */
  ///@{
  virtual double pdf(double x) const = 0;  ///< f(x); NaN if the family declines
  virtual double cdf(double x) const = 0;  ///< F(x); NaN if the family declines
  ///@}

  /** @brief Natural support interval of X. */
  virtual DistSupport support() const = 0;

  /**
   * @brief Finite window [lo, hi] covering essentially all of X's mass, for
   *        numerical quadrature.  Returns false (leaving @p lo / @p hi
   *        untouched) when the parameters are degenerate.
   */
  virtual bool integrationRange(double &lo, double &hi) const = 0;

  /**
   * @brief Plot x-window given optional truncation bounds (±infinity means
   *        "unbounded on that side"); used by the SVG curve renderer.
   */
  virtual std::pair<double, double> plotRange(double trunc_lo,
                                              double trunc_hi) const = 0;

  /** @brief Draw one sample using the shared MC generator. */
  virtual double sample(std::mt19937_64 &rng) const = 0;

  /**
   * @brief Closed-form location-scale transform @c a·X @c + @c b within
   *        the family.
   *
   * Returns the transformed distribution when the family is closed under
   * these coefficients, @c nullptr when it is not: Exponential / Erlang
   * decline @c a @c <= @c 0 (the support flips) and any non-zero offset
   * (a shifted Erlang leaves the family); every family declines
   * @c a @c == @c 0 (a Dirac is not in-family -- the constant-fold path
   * is responsible for pure constants).
   */
  virtual std::unique_ptr<Distribution> affine(double a, double b) const = 0;

  /** @brief Closed-form @c c·X (affine with no offset). */
  std::unique_ptr<Distribution> scale(double c) const {
    return affine(c, 0.0);
  }

  /** @brief Closed-form @c -X (affine with coefficient -1). */
  std::unique_ptr<Distribution> negate() const {
    return affine(-1.0, 0.0);
  }

  /**
   * @brief The on-disk @c gate_rv @c extra text encoding
   *        (e.g. <tt>"normal:2.5,0.5"</tt>), inverse of
   *        @c parse_distribution_spec.
   */
  virtual std::string serialise() const = 0;

  /**
   * @brief The point-mass value when the parameters make the
   *        distribution degenerate (a Dirac), @c std::nullopt for a
   *        proper distribution.
   *
   * Only Normal reports one (σ == 0, e.g. after an underflowing scale
   * fold); the simplifier collapses such a result to a plain constant.
   * Families whose degenerate forms are still sampled as-is
   * (Uniform(a, a)) do not report.
   */
  virtual std::optional<double> asDirac() const { return std::nullopt; }
};

/**
 * @brief Construct the per-family @c Distribution for a parsed spec.
 *
 * Returns @c nullptr only for an unknown @c DistKind (never for the four
 * built-in families).  Parameter-validity guards live in the family
 * methods (e.g. @c pdf returns NaN for a non-positive σ), matching the
 * pre-refactor behaviour.
 */
std::unique_ptr<Distribution> makeDistribution(const DistributionSpec &spec);

/**
 * @name ComparatorRuleRegistry — pairwise §B.2 closed forms
 *
 * Closed-form @f$P(X < Y)@f$ for an ordered pair of independent RV
 * families.  On continuous distributions every ordered comparator reduces
 * to @f$P(X < Y)@f$ or its complement, so rules are keyed on the family
 * pair alone (no operator in the key).  Pairwise behaviour deliberately
 * stays out of the @c Distribution interface: family files self-register
 * their rules at static initialisation through
 * @ref ComparatorRuleRegistrar, so adding a family touches no existing
 * file, and a missing rule is not an error -- the driver falls back to a
 * family-agnostic quadrature.
 */
///@{

/**
 * @brief A pairwise closed form for @f$P(X < Y)@f$, X and Y independent.
 *
 * Returns NaN when its parameter guards fail (e.g. a non-positive rate);
 * the driver then tries the generic quadrature.
 */
using ComparatorRule = double (*)(const Distribution &X,
                                  const Distribution &Y);

/** @brief Register the @f$P(X < Y)@f$ closed form for a family pair. */
void registerComparatorRule(DistKind x, DistKind y, ComparatorRule rule);

/** @brief Static-initialisation helper: one per registered family pair. */
struct ComparatorRuleRegistrar {
  ComparatorRuleRegistrar(DistKind x, DistKind y, ComparatorRule rule) {
    registerComparatorRule(x, y, rule);
  }
};

/**
 * @brief @f$P(X < Y)@f$ for two independent RVs.
 *
 * Applies the registered closed form for the family pair when there is
 * one; on a registry miss (or a rule declining with NaN) falls back to the
 * 1-D composite-Simpson quadrature
 * @f$P(X<Y) = \int (1 - F_Y(t))\, f_X(t)\, dt@f$ over X's integration
 * range.  NaN when neither decides (a density / CDF is undefined, e.g. a
 * non-integer Erlang shape), so the caller falls back to Monte Carlo.
 */
double comparatorPairLess(const Distribution &X, const Distribution &Y);

///@}

/**
 * @name ClosureRuleRegistry — pairwise family-closure folds on PLUS
 *
 * Closed-form folds of a sum of independent scalar terms into a single
 * distribution (Normal + Normal, same-rate Exponential / Erlang chains).
 * Like the comparator rules, pairwise behaviour stays out of the
 * @c Distribution interface: family files self-register at static
 * initialisation via @ref ClosureRuleRegistrar, keyed on the (ordered)
 * pair of families that may meet in the sum, and a registry miss simply
 * means the sum stays unfolded (Monte Carlo handles it).
 *
 * A rule receives the whole term list rather than reducing pairwise so
 * its accumulation arithmetic (e.g. one variance sum with a single final
 * square root) is not perturbed by intermediate re-serialisation.
 */
///@{

/**
 * @brief One wire of a PLUS under closure: @c a·Z @c + @c b for a base
 *        RV @c Z (@c dist non-null), or a pure additive constant @c b
 *        (@c dist null, @c a @c == @c 0).
 *
 * Structural concerns (base-RV identity, pairwise independence of the
 * @c Z's) are the caller's responsibility; rules only see distributions
 * and coefficients.
 */
struct ClosureTerm {
  const Distribution *dist;
  double a;
  double b;
};

/**
 * @brief A family sum-closure fold.  Returns the closed-form
 *        distribution of the summed terms, or @c nullptr when the shape
 *        is outside the closure (mixed rates, scaled / shifted terms a
 *        family cannot absorb, degenerate variance...).
 */
using ClosureRule =
  std::unique_ptr<Distribution> (*)(const std::vector<ClosureTerm> &terms);

/** @brief Register the sum-closure rule for a family pair. */
void registerClosureRule(DistKind x, DistKind y, ClosureRule rule);

/** @brief Static-initialisation helper: one per registered family pair. */
struct ClosureRuleRegistrar {
  ClosureRuleRegistrar(DistKind x, DistKind y, ClosureRule rule) {
    registerClosureRule(x, y, rule);
  }
};

/**
 * @brief Fold @c PLUS(terms) into a single distribution when a
 *        registered closure covers every family in the sum.
 *
 * Dispatch: the first RV term's family is looked up against itself and
 * against every other RV term's family; all lookups must resolve to the
 * same rule (this is how the Exponential / Erlang pairs share one
 * Erlang-sum rule).  Returns @c nullptr on any miss, on an
 * inconsistent pair, when no RV term is present (the constant-fold
 * path's job), or when the rule itself declines.
 */
std::unique_ptr<Distribution> closePlusTerms(
  const std::vector<ClosureTerm> &terms);

///@}

}  // namespace provsql

#endif  // PROVSQL_DISTRIBUTION_H
