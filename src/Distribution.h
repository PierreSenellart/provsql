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

}  // namespace provsql

#endif  // PROVSQL_DISTRIBUTION_H
