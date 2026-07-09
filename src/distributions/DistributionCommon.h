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

#include <cmath>
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

/// ln B(a, b) = lnΓ(a) + lnΓ(b) − lnΓ(a+b), for a, b > 0.
inline double lbeta(double a, double b)
{
  return std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b);
}

/// Standard normal pdf φ(z) = exp(-z²/2)/√(2π).
inline double phi(double z)
{
  static const double INV_SQRT_2PI = 1.0 / std::sqrt(2.0 * M_PI);
  return INV_SQRT_2PI * std::exp(-0.5 * z * z);
}

/// Standard normal CDF Φ(z) = ½(1 + erf(z/√2)).  Mirrors the
/// NormalDistribution::cdf convention so the truncation formulas
/// here use the same numerics.
inline double Phi(double z)
{
  static const double SQRT2 = std::sqrt(2.0);
  return 0.5 * (1.0 + std::erf(z / SQRT2));
}

/**
 * @brief Inverse standard-normal CDF, Beasley-Springer-Moro (1995).
 *
 * Returns @c z such that @f$\Phi(z) = p@f$.  Accurate to about
 * @c 1e-7 over @c p ∈ [0.02425, 1 - 0.02425], with a tail
 * rational fallback for the rest of @c (0, 1).  Callers must clamp
 * @c p strictly inside @c (0, 1) since the function diverges at the
 * endpoints; the truncated-normal sampler clamps to
 * @c [1e-15, 1 - 1e-15] before each call.
 *
 * The Beasley-Springer-Moro routine is in widespread library use
 * (NumPy/SciPy 'norminv', etc.) and its accuracy is several orders of
 * magnitude tighter than the sampling noise the tests can detect at
 * 10k draws, so it's a comfortable margin.
 */
inline double inv_phi(double p)
{
  static const double a[] = {
    -3.969683028665376e+01,  2.209460984245205e+02,
    -2.759285104469687e+02,  1.383577518672690e+02,
    -3.066479806614716e+01,  2.506628277459239e+00
  };
  static const double b[] = {
    -5.447609879822406e+01,  1.615858368580409e+02,
    -1.556989798598866e+02,  6.680131188771972e+01,
    -1.328068155288572e+01
  };
  static const double c_arr[] = {
    -7.784894002430293e-03, -3.223964580411365e-01,
    -2.400758277161838e+00, -2.549732539343734e+00,
     4.374664141464968e+00,  2.938163982698783e+00
  };
  static const double d[] = {
     7.784695709041462e-03,  3.224671290700398e-01,
     2.445134137142996e+00,  3.754408661907416e+00
  };
  static const double p_low  = 0.02425;
  static const double p_high = 1.0 - p_low;

  if (p < p_low) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
              + c_arr[3])*q + c_arr[4])*q + c_arr[5])
         / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
  }
  if (p <= p_high) {
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5]) * q
         / (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - p));
  return -(((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
             + c_arr[3])*q + c_arr[4])*q + c_arr[5])
        / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
}

/**
 * @brief Regularised lower incomplete gamma @f$P(a, x) = \gamma(a, x) /
 *        \Gamma(a)@f$ for @c a > 0, @c x >= 0.
 *
 * Series expansion of @f$\gamma(a, x)@f$ for @c x < @c a + 1, modified
 * Lentz continued fraction for the complement @f$Q(a, x)@f$ otherwise --
 * each converges fast in its region (Numerical Recipes §6.2).  NaN on
 * invalid @p a or non-convergence, so callers fall through to Monte
 * Carlo.
 */
inline double gammaP(double a, double x)
{
  if (!(a > 0.0) || std::isnan(x)) return kNaN;
  if (x <= 0.0) return 0.0;
  const double lg = std::lgamma(a);
  if (x < a + 1.0) {
    /* γ(a,x) = e^{-x} x^a Σ_{n≥0} x^n Γ(a)/Γ(a+1+n). */
    double ap = a, sum = 1.0 / a, del = sum;
    for (int n = 0; n < 500; ++n) {
      ap += 1.0;
      del *= x / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-15)
        return sum * std::exp(-x + a * std::log(x) - lg);
    }
    return kNaN;
  }
  /* Q(a,x) = e^{-x} x^a / Γ(a) · 1/(x+1-a- 1(1-a)/(x+3-a- ...)). */
  const double FPMIN = 1e-300;
  double b = x + 1.0 - a, c = 1.0 / FPMIN, d = 1.0 / b, h = d;
  for (int i = 1; i <= 500; ++i) {
    const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < FPMIN) d = FPMIN;
    c = b + an / c;
    if (std::fabs(c) < FPMIN) c = FPMIN;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-15) {
      const double q = std::exp(-x + a * std::log(x) - lg) * h;
      return 1.0 - q;
    }
  }
  return kNaN;
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
