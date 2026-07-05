/**
 * @file negative_binomial.cpp
 * @brief NegativeBinomial(r, p) family implementation (discrete).
 *
 * Number of failures before the r-th success, support @c {0, 1, 2, ...},
 * @c pmf(k) = C(k+r-1, k) p^r (1-p)^k (r real > 0 via the gamma form) -- the
 * same convention as the literal @c provsql.negative_binomial constructor.
 * Only ever instantiated for a @b latent leaf
 * @c provsql.negative_binomial(integer, random_variable); reached by
 * @c sample(), by @c observe (weight = pmf at the datum), and the moment
 * readouts.  Self-contained: registered at the bottom.
 */
#include "DistributionCommon.h"

#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

namespace provsql {

namespace {

/** @brief NegativeBinomial(r=p1, p=p2) on {0, 1, ...} (failures). */
class NegativeBinomialDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;

  bool valid() const { return p1_ > 0.0 && p2_ > 0.0 && p2_ <= 1.0; }

  /// log pmf(k) = lgamma(k+r) - lgamma(r) - lgamma(k+1) + r ln p + k ln(1-p).
  double logpmf(long k) const {
    const double r = p1_;
    return std::lgamma(k + r) - std::lgamma(r) - std::lgamma(k + 1.0)
         + r * std::log(p2_) + k * std::log(1.0 - p2_);
  }

  double mean() const override { return p1_ * (1.0 - p2_) / p2_; }   // r(1-p)/p
  bool isDiscrete() const override { return true; }
  double variance() const override {
    return p1_ * (1.0 - p2_) / (p2_ * p2_);
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (!valid()) return kNaN;
    // Σ_{j>=0} j^k pmf(j), truncated once the cumulative mass is essentially 1
    // past the mean; the cap bounds a pathologically heavy tail.
    double total = 0.0, cum = 0.0;
    const double mu = mean();
    for (long j = 0; j <= 100000000L; ++j) {
      const double pj = std::exp(logpmf(j));
      total += std::pow(static_cast<double>(j), static_cast<double>(k)) * pj;
      cum += pj;
      if (j > static_cast<long>(mu) && cum > 1.0 - 1e-15) break;
    }
    return total;
  }
  double pdf(double c) const override {   // pmf at an integer >= 0
    if (!valid()) return kNaN;
    const double r = std::nearbyint(c);
    if (std::fabs(c - r) > 1e-9 || r < 0.0) return 0.0;
    return std::exp(logpmf(static_cast<long>(r)));
  }
  double cdf(double c) const override {
    if (!valid()) return kNaN;
    if (c < 0.0) return 0.0;
    const long kmax = static_cast<long>(std::floor(c));
    double s = 0.0;
    for (long k = 0; k <= kmax; ++k) s += std::exp(logpmf(k));
    return s > 1.0 ? 1.0 : s;
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!valid()) return false;
    lo = 0.0;
    hi = mean() + 12.0 * std::sqrt(variance()) + 30.0;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo,
                                      double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = mean() + 4.0 * std::sqrt(variance()) + 5.0;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    // Gamma-Poisson mixture (valid for real r): λ ~ Gamma(r, scale=(1-p)/p),
    // then draw Poisson(λ).  Reproduces NegativeBinomial(r, p) for any r > 0.
    std::gamma_distribution<double> g(p1_, (1.0 - p2_) / p2_);
    const double lambda = g(rng);
    std::poisson_distribution<long> po(lambda > 0.0 ? lambda : 1e-12);
    return static_cast<double>(po(rng));
  }
  std::optional<double> quantile(double p) const override {
    if (!valid()) return std::nullopt;
    if (p <= 0.0) return 0.0;
    double s = 0.0;
    for (long k = 0; k <= 100000000L; ++k) {
      s += std::exp(logpmf(k));
      if (s >= p) return static_cast<double>(k);
    }
    return std::nullopt;
  }
  std::unique_ptr<Distribution> affine(double, double) const override {
    return nullptr;   // discrete: not closed under a·X + b
  }
  std::string serialise() const override {
    return "negative_binomial:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

const DistributionFamily negative_binomial_family = {
  "negative_binomial", 2, "NB", {"r", "p"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<NegativeBinomialDistribution>(p1, p2);
  }};

const DistributionFamily &NegativeBinomialDistribution::family() const
{
  return negative_binomial_family;
}

[[maybe_unused]] const DistributionFamilyRegistrar
  negative_binomial_family_registrar(negative_binomial_family);

}  // namespace

}  // namespace provsql
