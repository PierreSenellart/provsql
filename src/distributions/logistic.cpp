/**
 * @file logistic.cpp
 * @brief Logistic(μ, s) family implementation.
 *
 * The location-scale family whose CDF is the logistic sigmoid
 * @f$F(x) = \sigma((x-\mu)/s)@f$ and whose quantile is the logit.  This is
 * the natural noise for a latent-utility / logit-link selection model: a
 * threshold event @c eps @c < @c score with @c eps @c ~ @c Logistic(0,1)
 * has probability @f$\sigma(\text{score})@f$ exactly (a Normal @c eps would
 * give the probit @f$\Phi@f$, a different link off by a ~1.6 scale factor).
 *
 * Self-contained: the class is file-local and reaches the evaluators only
 * through the family registrar at the bottom (no comparator / closure rule
 * -- a difference of logistics is not logistic, so those fall through to the
 * family-agnostic quadrature / Monte Carlo).
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

/// E[Z^k] for Z ~ standard Logistic(0, 1): 0 for odd k (symmetric), and for
/// even k = 2n the value (2n)!·[t^{2n}] (πt/sin πt).  Tabulated up to k = 6
/// (covers mean / variance / skewness / kurtosis and a margin); kNaN beyond,
/// so a higher raw moment declines to Monte Carlo per the family contract.
double standard_logistic_moment(unsigned k)
{
  if (k % 2 == 1) return 0.0;
  const double pi2 = M_PI * M_PI;
  switch (k) {
    case 0: return 1.0;
    case 2: return pi2 / 3.0;                      // π²/3
    case 4: return 7.0 * pi2 * pi2 / 15.0;         // 7π⁴/15
    case 6: return 31.0 * pi2 * pi2 * pi2 / 21.0;  // 31π⁶/21
    default: return kNaN;
  }
}

/// Logistic sigmoid σ(z) = 1/(1+e^{-z}), evaluated without overflow.
double sigmoid(double z)
{
  if (z >= 0.0) return 1.0 / (1.0 + std::exp(-z));
  const double e = std::exp(z);
  return e / (1.0 + e);
}

/** @brief Logistic(μ=p1, s=p2). */
class LogisticDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override { return p1_; }
  bool meanIsAffine() const override { return true; }   // mean = μ
  double variance() const override {
    return M_PI * M_PI * p2_ * p2_ / 3.0;
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    // E[X^k] = Σ_{i=0}^{k} C(k,i) μ^{k-i} s^i E[Z^i], Z standard logistic.
    double total = 0.0;
    for (unsigned i = 0; i <= k; ++i) {
      const double ez = standard_logistic_moment(i);
      if (std::isnan(ez)) return kNaN;   // beyond the table: decline to MC
      if (ez == 0.0) continue;
      total += binomial_coeff(k, i)
             * std::pow(p1_, static_cast<double>(k - i))
             * std::pow(p2_, static_cast<double>(i))
             * ez;
    }
    return total;
  }
  double pdf(double c) const override {
    if (!(p2_ > 0.0)) return kNaN;
    // f(x) = σ(z)(1-σ(z))/s = e^{-|z|} / (s (1+e^{-|z|})²), z = (x-μ)/s.
    const double z = (c - p1_) / p2_;
    const double e = std::exp(-std::fabs(z));
    return e / ((1.0 + e) * (1.0 + e) * p2_);
  }
  double cdf(double c) const override {
    if (!(p2_ > 0.0)) return kNaN;
    return sigmoid((c - p1_) / p2_);
  }
  DistSupport support() const override { return {-kInf, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p2_ > 0.0)) return false;
    // The logistic tail F(μ-ks) = σ(-k) ≈ e^{-k}; ±30 s leaves < 1e-13.
    lo = p1_ - 30.0 * p2_;
    hi = p1_ + 30.0 * p2_;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo,
                                      double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = p1_ - 6.0 * p2_;
    if (!std::isfinite(hi)) hi = p1_ + 6.0 * p2_;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    double u = U01(rng);
    static constexpr double EPS = 1e-15;
    if (u < EPS) u = EPS;
    if (u > 1.0 - EPS) u = 1.0 - EPS;
    return p1_ + p2_ * std::log(u / (1.0 - u));   // μ + s·logit(u)
  }
  std::optional<double> quantile(double p) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    return p1_ + p2_ * std::log(p / (1.0 - p));    // μ + s·logit(p)
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    const double Fa = std::isfinite(lo) ? sigmoid((lo - p1_) / p2_) : 0.0;
    const double Fb = std::isfinite(hi) ? sigmoid((hi - p1_) / p2_) : 1.0;
    if (!(Fa < Fb)) return std::nullopt;
    static constexpr double EPS = 1e-15;
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    std::vector<double> out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
      double u = Fa + U01(rng) * (Fb - Fa);
      if (u < EPS) u = EPS;
      if (u > 1.0 - EPS) u = 1.0 - EPS;
      out.push_back(p1_ + p2_ * std::log(u / (1.0 - u)));
    }
    return out;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    if (a == 0.0) return nullptr;
    double mu = a * p1_;
    if (b != 0.0) mu += b;
    return std::make_unique<LogisticDistribution>(mu, std::fabs(a) * p2_);
  }
  std::string serialise() const override {
    return "logistic:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
  std::optional<double> asDirac() const override {
    if (p2_ == 0.0) return p1_;
    return std::nullopt;
  }
};

const DistributionFamily logistic_family = {
  "logistic", 2, "Lgs", {"μ", "s"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<LogisticDistribution>(p1, p2);
  }};

const DistributionFamily &LogisticDistribution::family() const
{
  return logistic_family;
}

[[maybe_unused]] const DistributionFamilyRegistrar logistic_family_registrar(
  logistic_family);

}  // namespace

}  // namespace provsql
