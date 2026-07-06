/**
 * @file geometric.cpp
 * @brief Geometric(p) family implementation (discrete).
 *
 * Number of trials until the first success, support @c {1, 2, 3, ...},
 * @c pmf(k) = (1-p)^{k-1} p -- the same convention as the literal
 * @c provsql.geometric constructor (which enumerates a categorical).  Only
 * ever instantiated for a @b latent leaf @c provsql.geometric(random_variable);
 * reached by @c sample(), by @c observe (weight = pmf at the datum), and the
 * moment readouts.  Self-contained: registered at the bottom.
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

/** @brief Geometric(p=p1) on {1, 2, ...}.  p2 unused. */
class GeometricDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;

  bool valid() const { return p1_ > 0.0 && p1_ <= 1.0; }

  double mean() const override { return 1.0 / p1_; }               // 1/p
  bool isDiscrete() const override { return true; }
  double variance() const override { return (1.0 - p1_) / (p1_ * p1_); }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    // Σ_{j>=1} j^k (1-p)^{j-1} p, truncated once the surviving tail mass
    // (1-p)^j is negligible; the cap bounds a pathologically small p.
    if (!valid()) return kNaN;
    const double q = 1.0 - p1_;
    double total = 0.0, term = p1_;   // term = pmf(j), starting j=1
    for (long j = 1; j <= 100000000L; ++j) {
      total += std::pow(static_cast<double>(j), static_cast<double>(k)) * term;
      term *= q;                       // pmf(j+1) = pmf(j)·(1-p)
      if (term < 1e-18) break;
    }
    return total;
  }
  double pdf(double c) const override {   // pmf at an integer >= 1
    if (!valid()) return kNaN;
    const double r = std::nearbyint(c);
    if (std::fabs(c - r) > 1e-9 || r < 1.0) return 0.0;
    return std::pow(1.0 - p1_, r - 1.0) * p1_;
  }
  double cdf(double c) const override {
    if (!valid()) return kNaN;
    if (c < 1.0) return 0.0;
    const double kf = std::floor(c);
    return 1.0 - std::pow(1.0 - p1_, kf);   // P(X <= k) = 1 - (1-p)^k
  }
  DistSupport support() const override { return {1.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!valid()) return false;
    lo = 1.0;
    // Smallest k with (1-p)^k < 1e-13 (all but a vanishing tail).
    hi = (p1_ >= 1.0) ? 1.0
       : 1.0 + std::ceil(std::log(1e-13) / std::log(1.0 - p1_));
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo,
                                      double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 1.0;
    if (!std::isfinite(hi)) {
      // integrationRange leaves its outputs untouched and returns false on an
      // invalid distribution; fall back to the (degenerate) lower bound then.
      double a = lo, b = lo;
      integrationRange(a, b);
      hi = b;
    }
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    // std::geometric_distribution counts failures on {0, 1, ...}; the trial
    // count (first success) is that plus one.
    std::geometric_distribution<long> d(p1_);
    return static_cast<double>(d(rng) + 1);
  }
  std::optional<double> quantile(double p) const override {
    if (!valid()) return std::nullopt;
    if (p <= 0.0) return 1.0;
    if (p1_ >= 1.0) return 1.0;
    // Smallest k with 1 - (1-p)^k >= p  <=>  k >= ln(1-p)/ln(1-p1).
    const double k = std::ceil(std::log(1.0 - p) / std::log(1.0 - p1_));
    return k < 1.0 ? 1.0 : k;
  }
  std::unique_ptr<Distribution> affine(double, double) const override {
    return nullptr;   // discrete: not closed under a·X + b
  }
  std::string serialise() const override {
    return "geometric:" + double_to_text(p1_);
  }
};

const DistributionFamily geometric_family = {
  "geometric", 1, "Geo", {"p", nullptr},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<GeometricDistribution>(p1, p2);
  }};

const DistributionFamily &GeometricDistribution::family() const
{
  return geometric_family;
}

[[maybe_unused]] const DistributionFamilyRegistrar geometric_family_registrar(
  geometric_family);

}  // namespace

}  // namespace provsql
