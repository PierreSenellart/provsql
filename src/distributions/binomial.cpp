/**
 * @file binomial.cpp
 * @brief Binomial(n, p) family implementation (discrete).
 *
 * A discrete, integer-valued family in the otherwise-continuous
 * @c Distribution hierarchy, instantiated only for a @b latent
 * (token-parameterised) leaf -- @c provsql.binomial(integer, random_variable)
 * -- the literal constructor enumerates an exact @c categorical instead.
 * As for @c poisson, a parametric leaf is declined by
 * @c parse_distribution_spec, so no continuous-analytic path integrates its
 * @c pdf as a density: it is reached only by the Monte Carlo @c sample(), by
 * @c observe (weight @c pdf = pmf at the datum), and by the moment path.
 *
 * @c p1 = n (number of trials), @c p2 = p (success probability).
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

/** @brief Binomial(n=p1, p=p2). */
class BinomialDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;

  long n() const { return static_cast<long>(std::llround(p1_)); }

  double mean() const override { return p1_ * p2_; }
  bool isDiscrete() const override { return true; }
  double variance() const override { return p1_ * p2_ * (1.0 - p2_); }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    // Exact finite sum Σ_{j=0}^{n} j^k · pmf(j).
    double total = 0.0;
    const long nn = n();
    for (long j = 0; j <= nn; ++j)
      total += std::pow(static_cast<double>(j), static_cast<double>(k)) * pmf(j);
    return total;
  }
  double pdf(double c) const override {   // pmf at a valid integer count
    if (!valid()) return kNaN;
    const double r = std::nearbyint(c);
    if (std::fabs(c - r) > 1e-9) return 0.0;
    const long k = static_cast<long>(r);
    if (k < 0 || k > n()) return 0.0;
    return pmf(k);
  }
  double cdf(double c) const override {
    if (!valid()) return kNaN;
    if (c < 0.0) return 0.0;
    const long nn = n();
    const long kmax = std::min(nn, static_cast<long>(std::floor(c)));
    double s = 0.0;
    for (long k = 0; k <= kmax; ++k) s += pmf(k);
    return s > 1.0 ? 1.0 : s;
  }
  DistSupport support() const override {
    return {0.0, static_cast<double>(n())};
  }
  bool integrationRange(double &lo, double &hi) const override {
    // Also the parameter-domain validity gate: n >= 0 and 0 <= p <= 1.
    if (!valid()) return false;
    lo = 0.0;
    hi = static_cast<double>(n());
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo,
                                      double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = static_cast<double>(n());
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::binomial_distribution<long> d(n(), p2_);
    return static_cast<double>(d(rng));
  }
  std::optional<double> quantile(double p) const override {
    if (!valid()) return std::nullopt;
    if (p <= 0.0) return 0.0;
    const long nn = n();
    double s = 0.0;
    for (long k = 0; k <= nn; ++k) {
      s += pmf(k);
      if (s >= p) return static_cast<double>(k);
    }
    return static_cast<double>(nn);
  }
  std::unique_ptr<Distribution> affine(double, double) const override {
    return nullptr;   // discrete: not closed under a·X + b
  }
  std::string serialise() const override {
    return "binomial:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }

private:
  bool valid() const {
    return std::isfinite(p1_) && p1_ >= 0.0 &&
           p2_ >= 0.0 && p2_ <= 1.0;
  }
  double pmf(long k) const {
    const long nn = n();
    if (k < 0 || k > nn) return 0.0;
    // Edge cases where std::log(p) / log(1-p) would be -inf.
    if (p2_ <= 0.0) return (k == 0) ? 1.0 : 0.0;
    if (p2_ >= 1.0) return (k == nn) ? 1.0 : 0.0;
    const double logC = std::lgamma(nn + 1.0) - std::lgamma(k + 1.0)
                      - std::lgamma(nn - k + 1.0);
    return std::exp(logC + k * std::log(p2_)
                    + (nn - k) * std::log1p(-p2_));
  }
};

const DistributionFamily binomial_family = {
  "binomial", 2, "Bin", {"n", "p"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<BinomialDistribution>(p1, p2);
  }};

const DistributionFamily &BinomialDistribution::family() const
{
  return binomial_family;
}

/* Beta-Binomial conjugacy: a Binomial(n, θ) observation with the latent
 * in the success-probability slot (n a known literal) updates a
 * Beta(α, β) prior to Beta(α+d, β+n−d).  The predictive is the
 * beta-binomial pmf m(d) = C(n,d) B(α+d, β+n−d) / B(α, β).  The count
 * must be an integer in [0, n] under the same 1e-9 rounding tolerance
 * as the family pmf. */
bool binomialPConjugateUpdate(double &alpha, double &beta,
                              const DistributionTemplate &lik, double d)
{
  const double n = lik.p1.literal;
  if (!(alpha > 0.0) || !(beta > 0.0)) return false;
  if (n < 1.0 || n != std::floor(n)) return false;
  const double r = std::nearbyint(d);
  if (std::fabs(d - r) > 1e-9 || r < 0.0 || r > n) return false;
  alpha += r;
  beta += n - r;
  return true;
}

double binomialPLogPredictive(double alpha, double beta,
                              const DistributionTemplate &lik, double d)
{
  const double n = lik.p1.literal;
  if (!(alpha > 0.0) || !(beta > 0.0)) return kNaN;
  if (n < 1.0 || n != std::floor(n)) return kNaN;
  const double r = std::nearbyint(d);
  if (std::fabs(d - r) > 1e-9 || r < 0.0 || r > n) return kNaN;
  return std::lgamma(n + 1.0) - std::lgamma(r + 1.0)
       - std::lgamma(n - r + 1.0)
       + lbeta(alpha + r, beta + n - r) - lbeta(alpha, beta);
}

[[maybe_unused]] const ConjugateRuleRegistrar binomial_p_conjugate(
  "binomial", 1, "beta",
  {&binomialPConjugateUpdate, &binomialPLogPredictive});

[[maybe_unused]] const DistributionFamilyRegistrar binomial_family_registrar(
  binomial_family);

}  // namespace

}  // namespace provsql
