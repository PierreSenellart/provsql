/**
 * @file beta.cpp
 * @brief Beta(α, β) family implementation on the unit interval:
 *        shapes @c α, @c β > 0.
 *
 * The conjugate prior of Bernoulli / binomial success probabilities and
 * the workhorse of bounded-quantity modelling.  The CDF is the
 * regularised incomplete beta @f$I_x(\alpha, \beta)@f$ (continued
 * fraction, file-local); the quantile rides the generic monotone-CDF
 * bisection over the finite @c [0, 1] support, and truncated moments
 * are closed-form through the moment-shifted incomplete beta.
 * @c Beta(1, 1) is @c Uniform(0, 1) and the SQL constructor routes it
 * there.
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom.
 */
#include "DistributionCommon.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace provsql {

namespace {

/**
 * @brief Regularised incomplete beta @f$I_x(a, b)@f$ for
 *        @c a, @c b > 0 and @c x in @c [0, 1].
 *
 * Modified Lentz continued fraction (Numerical Recipes §6.4), applied
 * directly for @c x < (a+1)/(a+b+2) and through the symmetry
 * @f$I_x(a,b) = 1 - I_{1-x}(b,a)@f$ otherwise so it always converges
 * fast.  NaN on invalid parameters or non-convergence, so callers fall
 * through to Monte Carlo.
 */
double betaCF(double a, double b, double x)
{
  const double FPMIN = 1e-300;
  const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
  double c = 1.0;
  double d = 1.0 - qab * x / qap;
  if (std::fabs(d) < FPMIN) d = FPMIN;
  d = 1.0 / d;
  double h = d;
  for (int m = 1; m <= 500; ++m) {
    const double m2 = 2.0 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1.0 + aa * d;
    if (std::fabs(d) < FPMIN) d = FPMIN;
    c = 1.0 + aa / c;
    if (std::fabs(c) < FPMIN) c = FPMIN;
    d = 1.0 / d;
    h *= d * c;
    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1.0 + aa * d;
    if (std::fabs(d) < FPMIN) d = FPMIN;
    c = 1.0 + aa / c;
    if (std::fabs(c) < FPMIN) c = FPMIN;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-15) return h;
  }
  return kNaN;
}

double betaI(double a, double b, double x)
{
  if (!(a > 0.0) || !(b > 0.0) || std::isnan(x)) return kNaN;
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  const double lbeta =
    std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b);
  const double front =
    std::exp(a * std::log(x) + b * std::log(1.0 - x) - lbeta);
  double result;
  if (x < (a + 1.0) / (a + b + 2.0))
    result = front * betaCF(a, b, x) / a;
  else
    result = 1.0 - front * betaCF(b, a, 1.0 - x) / b;
  return result;
}

/** @brief Beta(α=p1 > 0, β=p2 > 0) on [0, 1]. */
class BetaDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override { return p1_ / (p1_ + p2_); }
  double variance() const override {
    const double s = p1_ + p2_;
    return p1_ * p2_ / (s * s * (s + 1.0));
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    // E[X^k] = prod_{i<k} (α+i)/(α+β+i), a rising-factorial ratio.
    double r = 1.0;
    for (unsigned i = 0; i < k; ++i)
      r *= (p1_ + static_cast<double>(i))
         / (p1_ + p2_ + static_cast<double>(i));
    return r;
  }
  double pdf(double c) const override {
    const double a = p1_, b = p2_;
    if (!(a > 0.0) || !(b > 0.0)) return kNaN;
    if (c < 0.0 || c > 1.0) return 0.0;
    if (c == 0.0) {
      /* Integrable-singularity convention (as Gamma / Weibull at 0):
       * a shape below 1 diverges at the endpoint and reports NaN so
       * the uniform-grid quadratures decline to MC. */
      if (a < 1.0) return kNaN;
      return (a == 1.0) ? b : 0.0;
    }
    if (c == 1.0) {
      if (b < 1.0) return kNaN;
      return (b == 1.0) ? a : 0.0;
    }
    const double lbeta =
      std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b);
    return std::exp((a - 1.0) * std::log(c)
                    + (b - 1.0) * std::log(1.0 - c) - lbeta);
  }
  double cdf(double c) const override {
    return betaI(p1_, p2_, c);
  }
  DistSupport support() const override { return {0.0, 1.0}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0 && p2_ > 0.0)) return false;
    lo = 0.0;
    hi = 1.0;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = 1.0;
    return {std::max(lo, 0.0), std::min(hi, 1.0)};
  }
  double sample(std::mt19937_64 &rng) const override {
    /* No std::beta_distribution: the classic gamma-ratio construction,
     * X = G_α / (G_α + G_β) with unit-scale gammas. */
    std::gamma_distribution<double> ga(p1_, 1.0);
    std::gamma_distribution<double> gb(p2_, 1.0);
    const double x = ga(rng);
    const double y = gb(rng);
    return x / (x + y);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned k) const override {
    const double a = p1_, b = p2_;
    if (!(a > 0.0) || !(b > 0.0)) return std::nullopt;
    /* E[X^k · 1(lo<X<hi)] = (B(α+k, β)/B(α, β)) ·
     *   (I_hi(α+k, β) - I_lo(α+k, β)): the moment shifts α. */
    const double x_lo = std::isfinite(lo) ? std::max(lo, 0.0) : 0.0;
    const double x_hi = std::isfinite(hi) ? std::min(hi, 1.0) : 1.0;
    const double mass = betaI(a, b, x_hi) - betaI(a, b, x_lo);
    if (std::isnan(mass) || mass < 1e-12) return std::nullopt;
    if (k == 0) return 1.0;
    const double kd = static_cast<double>(k);
    const double ratio = std::exp(
      std::lgamma(a + kd) + std::lgamma(a + b)
      - std::lgamma(a) - std::lgamma(a + b + kd));
    const double shifted =
      betaI(a + kd, b, x_hi) - betaI(a + kd, b, x_lo);
    if (std::isnan(shifted)) return std::nullopt;
    return ratio * shifted / mass;
  }
  std::string serialise() const override {
    return "beta:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* A scaled or shifted Beta leaves the unit interval and the
     * family. */
    (void) a; (void) b;
    return nullptr;
  }
};

const DistributionFamily beta_family = {
  "beta", 2, "Β", {"α", "β"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<BetaDistribution>(p1, p2);
  }};

const DistributionFamily &BetaDistribution::family() const
{
  return beta_family;
}

[[maybe_unused]] const DistributionFamilyRegistrar beta_family_registrar(
  beta_family);

}  // namespace

}  // namespace provsql
