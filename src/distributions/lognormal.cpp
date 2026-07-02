/**
 * @file lognormal.cpp
 * @brief LogNormal(μ, σ) family implementation: @c exp of a
 *        Normal(μ, σ), parameterised by the underlying normal.
 *
 * The multiplicative counterpart of Normal: products of independent
 * lognormals are lognormal (parameters add in log space), positive
 * scalings shift μ by @c ln @c c, and the @c exp / @c ln transforms
 * bridge the two families -- all registered here as product / transform
 * / comparator rules, so the simplifier folds
 * <tt>exp(normal(...))</tt>, <tt>ln(lognormal(...))</tt>, and lognormal
 * products with no evaluator changes.
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom
 * (DistributionRegistry descriptor + pairwise / transform rules).
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

/** @brief LogNormal(μ=p1, σ=p2 of the underlying normal). */
class LogNormalDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override {
    return std::exp(p1_ + 0.5 * p2_ * p2_);
  }
  double variance() const override {
    const double s2 = p2_ * p2_;
    return std::expm1(s2) * std::exp(2.0 * p1_ + s2);
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    // E[X^k] = exp(kμ + k²σ²/2), finite for every k.
    const double kd = static_cast<double>(k);
    return std::exp(kd * p1_ + 0.5 * kd * kd * p2_ * p2_);
  }
  double pdf(double c) const override {
    if (!(p2_ > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    const double z = (std::log(c) - p1_) / p2_;
    return std::exp(-0.5 * z * z)
         / (c * p2_ * std::sqrt(2.0 * M_PI));
  }
  double cdf(double c) const override {
    if (!(p2_ > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    return Phi((std::log(c) - p1_) / p2_);
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p2_ > 0.0)) return false;
    /* [0, exp(μ + 8σ)] leaves Φ(-8) ≈ 6e-16 of mass outside.  The
     * multiplicative geometry means a uniform quadrature grid over this
     * window under-resolves the mass core for large σ (the window is
     * e^{8σ} times the median); the closed forms registered below keep
     * the common comparisons off that path. */
    lo = 0.0;
    hi = std::exp(p1_ + 8.0 * p2_);
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = std::exp(p1_ + 3.0 * p2_);  /* 99.87% */
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::lognormal_distribution<double> d(p1_, p2_);
    return d(rng);
  }
  std::optional<double> quantile(double p) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    /* exp of the normal quantile; same BSM + Newton polish. */
    double z = inv_phi(p);
    for (int i = 0; i < 2; ++i) {
      const double d = phi(z);
      if (!(d > 0.0)) break;
      z -= (Phi(z) - p) / d;
    }
    return std::exp(p1_ + p2_ * z);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned k) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    /* In log space the truncation is a normal truncation, and
     * E[X^k · 1(a<X<b)] integrates to the closed form
     * exp(kμ + k²σ²/2) · (Φ(β - kσ) - Φ(α - kσ)) with α, β the
     * standardised log bounds. */
    if (std::isfinite(hi) && hi <= 0.0) return std::nullopt;
    const double alpha = (std::isfinite(lo) && lo > 0.0)
      ? (std::log(lo) - p1_) / p2_ : -kInf;
    const double beta = std::isfinite(hi)
      ? (std::log(hi) - p1_) / p2_ : kInf;
    const double Phi_a = std::isfinite(alpha) ? Phi(alpha) : 0.0;
    const double Phi_b = std::isfinite(beta) ? Phi(beta) : 1.0;
    const double mass = Phi_b - Phi_a;
    if (mass < 1e-12) return std::nullopt;
    if (k == 0) return 1.0;
    const double kd = static_cast<double>(k);
    const double shifted_a =
      std::isfinite(alpha) ? Phi(alpha - kd * p2_) : 0.0;
    const double shifted_b =
      std::isfinite(beta) ? Phi(beta - kd * p2_) : 1.0;
    return std::exp(kd * p1_ + 0.5 * kd * kd * p2_ * p2_)
         * (shifted_b - shifted_a) / mass;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    /* Truncated normal in log space, exponentiated: the same
     * inverse-CDF transform as the Normal sampler over the
     * standardised log bounds. */
    if (std::isfinite(hi) && hi <= 0.0) return std::nullopt;
    const double alpha = (std::isfinite(lo) && lo > 0.0)
      ? (std::log(lo) - p1_) / p2_ : -kInf;
    const double beta = std::isfinite(hi)
      ? (std::log(hi) - p1_) / p2_ : kInf;
    const double Phi_a = std::isfinite(alpha) ? Phi(alpha) : 0.0;
    const double Phi_b = std::isfinite(beta) ? Phi(beta) : 1.0;
    if (!(Phi_a < Phi_b)) return std::nullopt;
    static constexpr double EPS = 1e-15;
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    std::vector<double> out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
      double u = Phi_a + U01(rng) * (Phi_b - Phi_a);
      if (u < EPS)       u = EPS;
      if (u > 1.0 - EPS) u = 1.0 - EPS;
      out.push_back(std::exp(p1_ + p2_ * inv_phi(u)));
    }
    return out;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* c · LogNormal(μ, σ) = LogNormal(μ + ln c, σ) for c > 0; a
     * negative scaling flips the support and an offset shifts it,
     * neither of which is lognormal. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    return std::make_unique<LogNormalDistribution>(p1_ + std::log(a), p2_);
  }
  std::string serialise() const override {
    return "lognormal:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
  std::optional<double> asDirac() const override {
    if (p2_ == 0.0) return std::exp(p1_);
    return std::nullopt;
  }
};

const DistributionFamily lognormal_family = {
  "lognormal", 2, "LogN", {"μ", "σ"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<LogNormalDistribution>(p1, p2);
  }};

const DistributionFamily &LogNormalDistribution::family() const
{
  return lognormal_family;
}

/* P(X < Y) for independent lognormals: monotone in log space, so it is
 * the Normal comparison of the underlying parameters,
 * Φ((μ_Y - μ_X) / √(σ_X² + σ_Y²)). */
double lognormalPairLess(const Distribution &X, const Distribution &Y)
{
  const double denom =
    std::sqrt(X.p2() * X.p2() + Y.p2() * Y.p2());
  return Phi((Y.p1() - X.p1()) / denom);
}

[[maybe_unused]] const ComparatorRuleRegistrar lognormal_less_rule(
  "lognormal", "lognormal", &lognormalPairLess);

/* Independent lognormals multiply to a lognormal: parameters add in log
 * space (Normal's sum closure seen through the exp bridge). */
std::unique_ptr<Distribution>
lognormalProductRule(const std::vector<const Distribution *> &factors)
{
  double mu = 0.0, var = 0.0;
  for (const Distribution *f : factors) {
    mu += f->p1();
    var += f->p2() * f->p2();
  }
  return std::make_unique<LogNormalDistribution>(mu, std::sqrt(var));
}

[[maybe_unused]] const ProductRuleRegistrar lognormal_product_rule(
  "lognormal", "lognormal", &lognormalProductRule);

/* The exp / ln bridges between the two families.  exp(Normal) is
 * constructed directly; ln(LogNormal) resolves Normal's factory through
 * the registry at rule-execution time (never at static init), so there
 * is no initialisation-order dependency on normal.cpp. */
std::unique_ptr<Distribution> expOfNormal(const Distribution &x)
{
  return std::make_unique<LogNormalDistribution>(x.p1(), x.p2());
}

std::unique_ptr<Distribution> lnOfLogNormal(const Distribution &x)
{
  const DistributionFamily *normal = lookupDistributionFamily("normal");
  return normal ? normal->factory(x.p1(), x.p2()) : nullptr;
}

[[maybe_unused]] const TransformRuleRegistrar exp_normal_rule(
  "exp", "normal", &expOfNormal);
[[maybe_unused]] const TransformRuleRegistrar ln_lognormal_rule(
  "ln", "lognormal", &lnOfLogNormal);

[[maybe_unused]] const DistributionFamilyRegistrar lognormal_family_registrar(
  lognormal_family);

}  // namespace

}  // namespace provsql
