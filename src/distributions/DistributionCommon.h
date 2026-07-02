/**
 * @file DistributionCommon.h
 * @brief Internal helpers shared by the per-family @c Distribution
 *        implementations under @c src/distributions/.
 *
 * Not part of the public @c Distribution interface -- include only from
 * the family implementation files.
 */
#ifndef PROVSQL_DISTRIBUTION_COMMON_H
#define PROVSQL_DISTRIBUTION_COMMON_H

#include <limits>

#include "Distribution.h"

namespace provsql {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline constexpr double kInf = std::numeric_limits<double>::infinity();

/// C(n, k) as a double (exact for the small moment orders used here).
inline double binomial_coeff(unsigned n, unsigned k)
{
  if (k > n) return 0.0;
  if (k > n - k) k = n - k;
  double r = 1.0;
  for (unsigned i = 1; i <= k; ++i) {
    r *= static_cast<double>(n - i + 1);
    r /= static_cast<double>(i);
  }
  return r;
}

/** @brief Base holding the two parameters; subclasses add closed forms. */
class BaseDistribution : public Distribution {
public:
  BaseDistribution(double p1, double p2) : p1_(p1), p2_(p2) {}
  double p1() const override { return p1_; }
  double p2() const override { return p2_; }
protected:
  double p1_, p2_;
};

}  // namespace provsql

#endif  // PROVSQL_DISTRIBUTION_COMMON_H
