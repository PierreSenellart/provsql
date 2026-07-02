/**
 * @file weibull.cpp
 * @brief Weibull(k, λ) family implementation: shape @c k > 0, scale
 *        @c λ > 0 (the 63.2% quantile -- NOT a rate; @c k = 1 is
 *        Exponential with rate @c 1/λ, and the SQL constructor routes
 *        that case through @c exponential).
 *
 * The workhorse of reliability / survival analysis: @c k tunes the
 * hazard (infant mortality for @c k < 1, wear-out for @c k > 1).
 * @f$(X/\lambda)^k@f$ is a unit exponential, which gives an exact
 * quantile, a same-shape comparator closed form, closed-form truncated
 * moments through the regularised incomplete gamma, and a min-stability
 * order statistic (the min of @c n i.i.d. Weibulls is Weibull at scale
 * @f$\lambda n^{-1/k}@f$).
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

/** @brief Weibull(k=p1 shape > 0, λ=p2 scale > 0). */
class WeibullDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override {
    return p2_ * std::tgamma(1.0 + 1.0 / p1_);
  }
  double variance() const override {
    const double g1 = std::tgamma(1.0 + 1.0 / p1_);
    return p2_ * p2_ * (std::tgamma(1.0 + 2.0 / p1_) - g1 * g1);
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    // E[X^j] = λ^j Γ(1 + j/k), finite for every j.
    const double j = static_cast<double>(k);
    return std::pow(p2_, j) * std::tgamma(1.0 + j / p1_);
  }
  double pdf(double c) const override {
    const double k = p1_, lambda = p2_;
    if (!(k > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c < 0.0) return 0.0;
    if (c == 0.0) {
      /* Same integrable-singularity convention as Gamma: shape < 1
       * diverges at 0 and reports NaN so the uniform-grid quadratures
       * decline to MC rather than integrate a singular endpoint. */
      if (k < 1.0) return kNaN;
      return (k == 1.0) ? 1.0 / lambda : 0.0;
    }
    const double u = std::pow(c / lambda, k);
    return (k / lambda) * std::pow(c / lambda, k - 1.0) * std::exp(-u);
  }
  double cdf(double c) const override {
    const double k = p1_, lambda = p2_;
    if (!(k > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    return -std::expm1(-std::pow(c / lambda, k));
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0 && p2_ > 0.0)) return false;
    /* quantile(1 - e^{-36}): mass beyond is ~2e-16. */
    lo = 0.0;
    hi = p2_ * std::pow(36.0, 1.0 / p1_);
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi))
      hi = p2_ * std::pow(6.907755278982137, 1.0 / p1_);  /* q(0.999) */
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::weibull_distribution<double> d(p1_, p2_);
    return d(rng);
  }
  std::optional<double> quantile(double p) const override {
    if (!(p1_ > 0.0) || !(p2_ > 0.0)) return std::nullopt;
    /* Exact inversion: λ·(-ln(1-p))^{1/k}, stable near p = 0 via
     * log1p. */
    return p2_ * std::pow(-std::log1p(-p), 1.0 / p1_);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned j) const override {
    const double k = p1_, lambda = p2_;
    if (!(k > 0.0) || !(lambda > 0.0)) return std::nullopt;
    /* Substituting u = (x/λ)^k turns the truncated moment into an
     * incomplete-gamma difference:
     * E[X^j · 1(a<X<b)] = λ^j Γ(1+j/k) (P(1+j/k, u_b) - P(1+j/k, u_a)). */
    const double u_a = (std::isfinite(lo) && lo > 0.0)
      ? std::pow(lo / lambda, k) : 0.0;
    const double u_b = std::isfinite(hi)
      ? (hi > 0.0 ? std::pow(hi / lambda, k) : 0.0) : kInf;
    const double mass = std::exp(-u_a) - std::exp(-u_b);
    if (mass < 1e-12) return std::nullopt;
    if (j == 0) return 1.0;
    const double a = 1.0 + static_cast<double>(j) / k;
    const double P_a = u_a > 0.0 ? gammaP(a, u_a) : 0.0;
    const double P_b = std::isfinite(u_b) ? gammaP(a, u_b) : 1.0;
    if (std::isnan(P_a) || std::isnan(P_b)) return std::nullopt;
    return std::pow(lambda, static_cast<double>(j)) * std::tgamma(a)
         * (P_b - P_a) / mass;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    const double k = p1_, lambda = p2_;
    if (!(k > 0.0) || !(lambda > 0.0)) return std::nullopt;
    std::vector<double> out;
    out.reserve(n);
    const double u_lo = (std::isfinite(lo) && lo > 0.0)
      ? std::pow(lo / lambda, k) : 0.0;
    if (std::isinf(hi)) {
      /* One-sided: (X/λ)^k is a unit exponential, which is memoryless,
       * so u | u > u_lo = u_lo + Exp(1) -- numerically stable for
       * arbitrarily deep tails. */
      std::exponential_distribution<double> E(1.0);
      for (unsigned i = 0; i < n; ++i)
        out.push_back(lambda * std::pow(u_lo + E(rng), 1.0 / k));
      return out;
    }
    /* Two-sided: exact inverse CDF on [F(lo), F(hi)]. */
    const double F_lo = -std::expm1(-u_lo);
    const double F_hi = hi > 0.0
      ? -std::expm1(-std::pow(hi / lambda, k)) : 0.0;
    if (!(F_lo < F_hi)) return std::nullopt;
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    for (unsigned i = 0; i < n; ++i) {
      const double u = F_lo + U01(rng) * (F_hi - F_lo);
      out.push_back(lambda * std::pow(-std::log1p(-u), 1.0 / k));
    }
    return out;
  }
  std::optional<double> iidOrderStatMean(std::size_t n,
                                         bool isMax) const override {
    if (!(p1_ > 0.0) || !(p2_ > 0.0)) return std::nullopt;
    if (isMax) return std::nullopt;   /* no elementary max closed form */
    /* Min-stability: min of n i.i.d. Weibull(k, λ) is
     * Weibull(k, λ n^{-1/k}). */
    const double scale =
      p2_ * std::pow(static_cast<double>(n), -1.0 / p1_);
    return scale * std::tgamma(1.0 + 1.0 / p1_);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* Positive scalings rescale λ; negations / offsets leave the
     * family. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    return std::make_unique<WeibullDistribution>(p1_, a * p2_);
  }
  std::string serialise() const override {
    return "weibull:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

const DistributionFamily weibull_family = {
  "weibull", 2, "Wbl", {"k", "λ"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<WeibullDistribution>(p1, p2);
  }};

const DistributionFamily &WeibullDistribution::family() const
{
  return weibull_family;
}

/* P(X < Y) for independent same-shape Weibulls: (X/1)^k maps both to
 * exponentials, so P(X < Y) = λ_Y^k / (λ_X^k + λ_Y^k).  Different
 * shapes have no elementary form; NaN falls to the quadrature. */
double weibullPairLess(const Distribution &X, const Distribution &Y)
{
  if (X.p1() != Y.p1()) return kNaN;
  const double ax = std::pow(X.p2(), X.p1());
  const double ay = std::pow(Y.p2(), Y.p1());
  if (!(ax > 0.0) || !(ay > 0.0)) return kNaN;
  return ay / (ax + ay);
}

[[maybe_unused]] const ComparatorRuleRegistrar weibull_less_rule(
  "weibull", "weibull", &weibullPairLess);

[[maybe_unused]] const DistributionFamilyRegistrar weibull_family_registrar(
  weibull_family);

}  // namespace

}  // namespace provsql
