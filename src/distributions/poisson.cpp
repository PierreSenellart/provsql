/**
 * @file poisson.cpp
 * @brief Poisson(λ) family implementation (discrete).
 *
 * A discrete, integer-valued family in the otherwise-continuous
 * @c Distribution hierarchy.  It is only ever instantiated for a
 * @b latent (token-parameterised) leaf -- @c provsql.poisson(random_variable)
 * -- because the literal-parameter constructor enumerates an exact
 * @c categorical instead.  A parametric leaf is declined by
 * @c parse_distribution_spec, so no continuous-analytic path ever integrates
 * this family's @c pdf as a density: it is reached only by the Monte Carlo
 * @c sample(), by @c observe (whose weight is @c pdf = the pmf at the datum),
 * and by the affine-mean fast path (@c mean = λ, @c meanIsAffine() = true).
 *
 * Self-contained: the class is file-local and reaches the evaluators only
 * through the DistributionRegistry registrar at the bottom.
 */
#include "DistributionCommon.h"

#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace provsql {

namespace {

/** @brief Poisson(λ=p1). p2 unused. */
class PoissonDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override { return p1_; }
  bool meanIsAffine() const override { return true; }   // mean = λ
  bool isDiscrete() const override { return true; }
  double variance() const override { return p1_; }
  double rawMoment(unsigned k) const override {
    // Touchard recurrence: m_0 = 1, m_{j} = λ Σ_{i=0}^{j-1} C(j-1,i) m_i.
    if (k == 0) return 1.0;
    std::vector<double> m(k + 1, 0.0);
    m[0] = 1.0;
    for (unsigned j = 1; j <= k; ++j) {
      double s = 0.0;
      for (unsigned i = 0; i < j; ++i) s += binomial_coeff(j - 1, i) * m[i];
      m[j] = p1_ * s;
    }
    return m[k];
  }
  double pdf(double c) const override {   // pmf: mass at a non-negative integer
    if (!(p1_ > 0.0)) return kNaN;
    const double r = std::nearbyint(c);
    if (std::fabs(c - r) > 1e-9 || r < 0.0) return 0.0;
    return std::exp(-p1_ + r * std::log(p1_) - std::lgamma(r + 1.0));
  }
  double cdf(double c) const override {
    if (!(p1_ > 0.0)) return kNaN;
    if (c < 0.0) return 0.0;
    const long kmax = static_cast<long>(std::floor(c));
    double s = 0.0;
    for (long k = 0; k <= kmax; ++k)
      s += std::exp(-p1_ + k * std::log(p1_) - std::lgamma(k + 1.0));
    return s > 1.0 ? 1.0 : s;
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    // Also the parameter-domain validity gate: λ must be > 0.
    if (!(p1_ > 0.0)) return false;
    lo = 0.0;
    hi = p1_ + 12.0 * std::sqrt(p1_) + 30.0;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo,
                                      double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = p1_ + 4.0 * std::sqrt(p1_) + 5.0;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::poisson_distribution<long> d(p1_);
    return static_cast<double>(d(rng));
  }
  std::optional<double> quantile(double p) const override {
    if (!(p1_ > 0.0)) return std::nullopt;
    if (p <= 0.0) return 0.0;
    double s = 0.0;
    for (long k = 0; k < 1000000; ++k) {
      s += std::exp(-p1_ + k * std::log(p1_) - std::lgamma(k + 1.0));
      if (s >= p) return static_cast<double>(k);
    }
    return std::nullopt;
  }
  std::unique_ptr<Distribution> affine(double, double) const override {
    return nullptr;   // discrete: not closed under a·X + b
  }
  std::string serialise() const override {
    return "poisson:" + double_to_text(p1_);
  }
};

const DistributionFamily poisson_family = {
  "poisson", 1, "Pois", {"λ", nullptr},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<PoissonDistribution>(p1, p2);
  }};

const DistributionFamily &PoissonDistribution::family() const
{
  return poisson_family;
}

/* Gamma-Poisson conjugacy: a Poisson(θ) count observation of a
 * Gamma(k, λ) prior on the rate updates to Gamma(k+d, λ+1).  The
 * predictive is the negative-binomial pmf
 * m(d) = Γ(k+d)/(Γ(k) d!) · λ^k / (λ+1)^{k+d}.  The datum must be a
 * non-negative integer under the same 1e-9 rounding tolerance the
 * family pmf applies, so the closed form fires exactly where the
 * importance weight is positive. */
bool poissonRateConjugateUpdate(double &k, double &lambda,
                                const DistributionTemplate &, double d)
{
  if (!(k > 0.0) || !(lambda > 0.0)) return false;
  const double r = std::nearbyint(d);
  if (std::fabs(d - r) > 1e-9 || r < 0.0) return false;
  k += r;
  lambda += 1.0;
  return true;
}

double poissonRateLogPredictive(double k, double lambda,
                                const DistributionTemplate &, double d)
{
  if (!(k > 0.0) || !(lambda > 0.0)) return kNaN;
  const double r = std::nearbyint(d);
  if (std::fabs(d - r) > 1e-9 || r < 0.0) return kNaN;
  return std::lgamma(k + r) - std::lgamma(k) - std::lgamma(r + 1.0)
       + k * std::log(lambda) - (k + r) * std::log(lambda + 1.0);
}

[[maybe_unused]] const ConjugateRuleRegistrar poisson_rate_conjugate(
  "poisson", 0, "gamma",
  {&poissonRateConjugateUpdate, &poissonRateLogPredictive});

[[maybe_unused]] const DistributionFamilyRegistrar poisson_family_registrar(
  poisson_family);

}  // namespace

}  // namespace provsql
