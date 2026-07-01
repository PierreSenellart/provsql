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
#include <random>
#include <string>
#include <utility>

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

}  // namespace provsql

#endif  // PROVSQL_DISTRIBUTION_H
