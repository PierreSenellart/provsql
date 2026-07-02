/**
 * @file exponential.cpp
 * @brief Exponential(λ) family implementation.
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom
 * (DistributionRegistry factory + pairwise comparator / closure rules).
 * The closed forms were relocated verbatim from the pre-refactor
 * @c DistKind switches, so behaviour is preserved by construction.
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

double factorial(unsigned k)
{
  double r = 1.0;
  for (unsigned i = 2; i <= k; ++i) r *= static_cast<double>(i);
  return r;
}

/**
 * @brief Raw moments of @c X ~ Exp(λ) truncated to @c [a, b].
 *
 * Decomposes via change of variable Y = X - max(a,0):
 *   - left endpoint @c a > 0, right endpoint @c b = +∞: by
 *     memorylessness @c X | X>a is distributed as @c a + Exp(λ), so
 *     @c E[X^k|X>a] = Σ_{i=0..k} C(k,i) a^{k-i} · i!/λ^i.
 *   - finite @c [a, b] (with @c a ≥ 0, @c b < ∞): integrate
 *     @c x^k λ e^{-λx} dx by parts and divide by the truncation mass
 *     @c e^{-λa} - e^{-λb}.  Uses the recurrence
 *     @c I_k = k I_{k-1} / λ - (b^k e^{-λb} - a^k e^{-λa}) / λ
 *     with @c I_0 = e^{-λa} - e^{-λb}.
 */
double truncated_exponential_raw_moment(double lambda, double a, double b,
                                        unsigned k)
{
  const double aa = std::max(a, 0.0);  /* Exp support is [0, +∞) */
  if (std::isfinite(b)) {
    if (b <= aa) return kNaN;
    /* Finite-interval recurrence on I_k = ∫_{aa}^{b} x^k λ e^{-λx} dx. */
    const double e_a = std::exp(-lambda * aa);
    const double e_b = std::exp(-lambda * b);
    const double Z = e_a - e_b;  /* P(aa < X < b) */
    if (Z < 1e-12) return kNaN;
    if (k == 0) return 1.0;
    /* Integration by parts: ∫ x^k λ e^{-λx} dx = -x^k e^{-λx} + k ∫ x^{k-1} e^{-λx} dx
     * so I_k (with λ factor folded into the e^{-λx}·λ dx term) follows:
     *   I_k = [aa^k e^{-λaa} - b^k e^{-λb}] + (k/λ) · I_{k-1}_no_lambda
     * where I_{k-1}_no_lambda = ∫ x^{k-1} e^{-λx} dx = I_{k-1}/λ.
     * Cleaner: compute J_k = ∫_{aa}^{b} x^k e^{-λx} dx via
     *   J_0 = Z/λ; J_k = (aa^k e^{-λaa} - b^k e^{-λb})/λ + (k/λ) J_{k-1}.
     * Then E[X^k|aa<X<b] = λ J_k / Z. */
    std::vector<double> J(k + 1, 0.0);
    J[0] = Z / lambda;
    for (unsigned m = 1; m <= k; ++m) {
      const double endpoint = std::pow(aa, static_cast<double>(m)) * e_a
                            - std::pow(b,  static_cast<double>(m)) * e_b;
      J[m] = endpoint / lambda + (m / lambda) * J[m - 1];
    }
    return lambda * J[k] / Z;
  }
  /* Semi-infinite right tail [aa, +∞): memorylessness. */
  double total = 0.0;
  double fact_i = 1.0;
  for (unsigned i = 0; i <= k; ++i) {
    total += binomial_coeff(k, i)
           * std::pow(aa, static_cast<double>(k - i))
           * fact_i / std::pow(lambda, static_cast<double>(i));
    fact_i *= (i + 1);
  }
  return total;
}

/** @brief Exponential(λ=p1). p2 unused. */
class ExponentialDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  DistKind kind() const override { return DistKind::Exponential; }
  double mean() const override { return 1.0 / p1_; }
  double variance() const override { return 1.0 / (p1_ * p1_); }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    return factorial(k) / std::pow(p1_, static_cast<double>(k));
  }
  double pdf(double c) const override {
    if (!(p1_ > 0.0)) return kNaN;
    return (c < 0.0) ? 0.0 : p1_ * std::exp(-p1_ * c);
  }
  double cdf(double c) const override {
    return (c <= 0.0) ? 0.0 : -std::expm1(-p1_ * c);  // 1 - exp(-λc)
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0)) return false;
    lo = 0.0;
    hi = 40.0 / p1_;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = 6.0 / p1_;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::exponential_distribution<double> d(p1_);
    return d(rng);
  }
  std::optional<double> quantile(double p) const override {
    if (!(p1_ > 0.0)) return std::nullopt;
    /* -log1p(-p)/λ for accuracy as p approaches 1. */
    return -std::log1p(-p) / p1_;
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned k) const override {
    const double r = truncated_exponential_raw_moment(p1_, lo, hi, k);
    if (std::isnan(r)) return std::nullopt;
    return r;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    const double lambda = p1_;
    if (!(lambda > 0.0)) return std::nullopt;
    std::vector<double> out;
    out.reserve(n);
    if (std::isinf(hi)) {
      /* X | X > lo = lo + Exp(λ) by memorylessness.  Numerically
       * stable for arbitrarily large @c lo, where the inverse-CDF
       * would underflow on @c 1 - exp(-λ·lo). */
      std::exponential_distribution<double> E(lambda);
      for (unsigned i = 0; i < n; ++i) out.push_back(lo + E(rng));
      return out;
    }
    /* Two-sided truncation: inverse-CDF on @c [F(lo), F(hi)] with
     * @c F(x) = -expm1(-λx) for accuracy near 0, and @c x =
     * -log1p(-u)/λ for accuracy as @c u approaches 1. */
    const double F_lo = -std::expm1(-lambda * lo);
    const double F_hi = -std::expm1(-lambda * hi);
    if (!(F_lo < F_hi)) return std::nullopt;
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    for (unsigned i = 0; i < n; ++i) {
      const double u = F_lo + U01(rng) * (F_hi - F_lo);
      out.push_back(-std::log1p(-u) / lambda);
    }
    return out;
  }
  std::optional<double> iidOrderStatMean(std::size_t n,
                                         bool isMax) const override {
    const double lambda = p1_;
    if (!(lambda > 0.0))
      return std::nullopt;
    if (!isMax)
      return 1.0 / (static_cast<double>(n) * lambda);  /* min of exps */
    double H = 0.0;                                     /* max: harmonic */
    for (std::size_t k = 1; k <= n; ++k)
      H += 1.0 / static_cast<double>(k);
    return H / lambda;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* Closed under positive scaling only: a negative coefficient flips
     * the support to (-∞, 0] and an offset shifts it, neither of which
     * is exponential. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    return std::make_unique<ExponentialDistribution>(p1_ / a, 0.0);
  }
  std::string serialise() const override {
    return "exponential:" + double_to_text(p1_);
  }
};

/* P(X < Y) = λ_X / (λ_X + λ_Y) for independent exponentials. */
double exponentialPairLess(const Distribution &X, const Distribution &Y)
{
  const double lx = X.p1(), ly = Y.p1();
  if (!(lx > 0.0 && ly > 0.0)) return kNaN;
  return lx / (lx + ly);
}

[[maybe_unused]] const ComparatorRuleRegistrar exponential_less_rule(
  DistKind::Exponential, DistKind::Exponential, &exponentialPairLess);

[[maybe_unused]] const DistributionFamilyRegistrar exponential_family(
  DistKind::Exponential, "exponential", 1,
  [](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<ExponentialDistribution>(p1, p2);
  });

}  // namespace

}  // namespace provsql
