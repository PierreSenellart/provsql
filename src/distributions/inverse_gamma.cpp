/**
 * @file inverse_gamma.cpp
 * @brief InvGamma(α, β) family implementation: shape @c α > 0, scale
 *        @c β > 0.
 *
 * The distribution of @c 1/Y for @c Y ~ Gamma(shape α, rate β): the
 * conjugate prior for a Gaussian variance and a common heavy-tailed
 * model for positive quantities.  The CDF is the regularised @b upper
 * incomplete gamma @f$Q(α, β/x)@f$, so it reuses the same @c gammaP
 * kernel as Gamma; raw moments @c E[X^k] @c = @c β^k Γ(α-k)/Γ(α) are
 * @b infinite for @c α <= @c k and reported honestly as @c +Infinity
 * (the mean for @c α <= @c 1, the variance for @c α <= @c 2) rather than
 * estimated.  Positive scalings rescale @c β (scale family); the
 * quantile / truncated-moment paths decline (no elementary inverse CDF)
 * and fall through to the numeric bisection / Monte Carlo, exactly as
 * Gamma does.
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom
 * (DistributionRegistry factory).  No evaluator or parser code changes.
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

/** @brief InvGamma(α=p1 shape > 0, β=p2 scale > 0). */
class InverseGammaDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override {
    if (!(p1_ > 1.0)) return kInf;   /* diverges for α <= 1 */
    return p2_ / (p1_ - 1.0);
  }
  double variance() const override {
    if (!(p1_ > 2.0)) return kInf;   /* diverges for α <= 2 */
    const double am1 = p1_ - 1.0;
    return p2_ * p2_ / (am1 * am1 * (p1_ - 2.0));
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    const double alpha = p1_;
    const double kd = static_cast<double>(k);
    if (!(alpha > kd)) return kInf;  /* E[X^k] diverges for α <= k */
    /* β^k Γ(α-k)/Γ(α) = β^k / [(α-1)(α-2)...(α-k)], every factor > 0. */
    double denom = 1.0;
    for (unsigned i = 1; i <= k; ++i) denom *= (alpha - static_cast<double>(i));
    return std::pow(p2_, kd) / denom;
  }
  double pdf(double c) const override {
    const double alpha = p1_, beta = p2_;
    if (!(alpha > 0.0) || !(beta > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    /* β^α/Γ(α) · x^{-α-1} · e^{-β/x}, computed in log space. */
    return std::exp(alpha * std::log(beta) - std::lgamma(alpha)
                    - (alpha + 1.0) * std::log(c) - beta / c);
  }
  double cdf(double c) const override {
    const double alpha = p1_, beta = p2_;
    if (!(alpha > 0.0) || !(beta > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    /* F(x) = P(1/X <= 1/x)ᶜ = Q(α, β/x) = 1 - P(α, β/x). */
    const double p = gammaP(alpha, beta / c);
    if (std::isnan(p)) return kNaN;
    return 1.0 - p;
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    const double alpha = p1_, beta = p2_;
    if (!(alpha > 0.0 && beta > 0.0)) return false;
    lo = 0.0;
    if (alpha > 2.0) {
      /* Finite variance: the Gamma-style mean + 12σ window. */
      const double m = beta / (alpha - 1.0);
      const double sd =
        std::sqrt(beta * beta / ((alpha - 1.0) * (alpha - 1.0) * (alpha - 2.0)));
      hi = m + 12.0 * sd;
    } else {
      /* Heavy right tail (Pareto-like exponent α): the tail
       * approximation q(1 - 1e-9) ≈ β·(1e9)^{1/α}, generously wide. */
      hi = beta * std::pow(1e9, 1.0 / alpha);
    }
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) {
      if (p1_ > 2.0) {
        const double m = p2_ / (p1_ - 1.0);
        const double sd = std::sqrt(
          p2_ * p2_ / ((p1_ - 1.0) * (p1_ - 1.0) * (p1_ - 2.0)));
        hi = m + 4.0 * sd;
      } else {
        hi = p2_ * std::pow(1e3, 1.0 / p1_);   /* q(0.999) */
      }
    }
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    /* 1/Y for Y ~ Gamma(shape α, scale 1/β) = Gamma(shape α, rate β). */
    std::gamma_distribution<double> d(p1_, 1.0 / p2_);
    const double y = d(rng);
    return 1.0 / y;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* c · InvGamma(α, β) = InvGamma(α, c·β) for c > 0; a negative
     * scaling flips the support and an offset shifts it, neither of
     * which is inverse-gamma. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    if (!(p1_ > 0.0)) return nullptr;
    return std::make_unique<InverseGammaDistribution>(p1_, a * p2_);
  }
  std::string serialise() const override {
    return "inverse_gamma:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

const DistributionFamily inverse_gamma_family = {
  "inverse_gamma", 2, "IΓ", {"α", "β"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<InverseGammaDistribution>(p1, p2);
  }};

const DistributionFamily &InverseGammaDistribution::family() const
{
  return inverse_gamma_family;
}

[[maybe_unused]] const DistributionFamilyRegistrar inverse_gamma_family_registrar(
  inverse_gamma_family);

}  // namespace

}  // namespace provsql
