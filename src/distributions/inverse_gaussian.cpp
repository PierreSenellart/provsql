/**
 * @file inverse_gaussian.cpp
 * @brief InverseGaussian(μ, λ) / Wald family implementation: mean
 *        @c μ > 0, shape @c λ > 0.
 *
 * The first-passage time of Brownian motion with drift: a positive,
 * right-skewed family widely used for reliability and reaction-time
 * models.  Its CDF has a closed form in terms of the standard normal
 * @c Φ, so @f$P(X < c)@f$ and the numeric quantile are analytic; all raw
 * moments are finite (a finite-sum closed form).  Positive scalings map
 * @c c·IG(μ, λ) @c = @c IG(cμ, cλ), and a sum of independent inverse
 * Gaussians that share the ratio @c λ/μ² folds to a single inverse
 * Gaussian in the simplifier (registered as a closure rule below).
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom
 * (DistributionRegistry factory + sum-closure rule).  No evaluator or
 * parser code changes.
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

/** @brief n! as a double (exact for the small moment orders used here). */
double factorial(unsigned n)
{
  double r = 1.0;
  for (unsigned i = 2; i <= n; ++i) r *= static_cast<double>(i);
  return r;
}

/** @brief InverseGaussian(μ=p1 mean > 0, λ=p2 shape > 0). */
class InverseGaussianDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override { return p1_; }
  bool meanIsAffine() const override { return true; }   // mean = μ
  double variance() const override { return p1_ * p1_ * p1_ / p2_; }
  double rawMoment(unsigned n) const override {
    if (n == 0) return 1.0;
    if (n == 1) return p1_;
    const double mu = p1_, lambda = p2_;
    /* E[X^n] = μ^n Σ_{s=0}^{n-1} (n-1+s)!/(s!(n-1-s)!) · (μ/2λ)^s. */
    const double r = mu / (2.0 * lambda);
    double sum = 0.0, rpow = 1.0;
    for (unsigned s = 0; s < n; ++s) {
      const double coef =
        factorial(n - 1 + s) / (factorial(s) * factorial(n - 1 - s));
      sum += coef * rpow;
      rpow *= r;
    }
    return std::pow(mu, static_cast<double>(n)) * sum;
  }
  double pdf(double c) const override {
    const double mu = p1_, lambda = p2_;
    if (!(mu > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    const double d = c - mu;
    return std::sqrt(lambda / (2.0 * M_PI * c * c * c))
         * std::exp(-lambda * d * d / (2.0 * mu * mu * c));
  }
  double cdf(double c) const override {
    const double mu = p1_, lambda = p2_;
    if (!(mu > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    const double s = std::sqrt(lambda / c);
    const double term1 = Phi(s * (c / mu - 1.0));
    /* exp(2λ/μ) · Φ(-√(λ/c)(c/μ + 1)): the exponential overflows for
     * large λ/μ while its Φ factor underflows; their product tends to 0,
     * so guard the inf·0 with a finite-check. */
    double term2 = std::exp(2.0 * lambda / mu) * Phi(-s * (c / mu + 1.0));
    if (!std::isfinite(term2)) term2 = 0.0;
    return term1 + term2;
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0 && p2_ > 0.0)) return false;
    /* Mean μ + 12 standard deviations √(μ³/λ). */
    lo = 0.0;
    hi = p1_ + 12.0 * std::sqrt(p1_ * p1_ * p1_ / p2_);
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi))
      hi = p1_ + 4.0 * std::sqrt(p1_ * p1_ * p1_ / p2_);
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    /* Michael-Schucany-Haas (1976): one chi-square draw + a Bernoulli
     * accept/reflect on the two roots. */
    const double mu = p1_, lambda = p2_;
    std::normal_distribution<double> N01(0.0, 1.0);
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    const double nu = N01(rng);
    const double y = nu * nu;
    const double x = mu + (mu * mu * y) / (2.0 * lambda)
      - (mu / (2.0 * lambda))
        * std::sqrt(4.0 * mu * lambda * y + mu * mu * y * y);
    if (U01(rng) <= mu / (mu + x)) return x;
    return mu * mu / x;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* c · IG(μ, λ) = IG(cμ, cλ) for c > 0; negations flip the support
     * and offsets shift it, neither of which is inverse-Gaussian. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    return std::make_unique<InverseGaussianDistribution>(a * p1_, a * p2_);
  }
  std::string serialise() const override {
    return "inverse_gaussian:" + double_to_text(p1_) + ","
         + double_to_text(p2_);
  }
};

const DistributionFamily inverse_gaussian_family = {
  "inverse_gaussian", 2, "IG", {"μ", "λ"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<InverseGaussianDistribution>(p1, p2);
  }};

const DistributionFamily &InverseGaussianDistribution::family() const
{
  return inverse_gaussian_family;
}

/* (IG, +, IG): a sum of independent inverse Gaussians that all share the
 * ratio φ = λ/μ² folds to IG(Σμ, φ·(Σμ)²).  Strict shape like the
 * Gamma-sum rule: unscaled, unshifted, no additive constants, ratios
 * exactly equal.  A mismatch (or a scaled / shifted term) declines, and
 * the sum stays unfolded for Monte Carlo -- only the closed form is
 * missed. */
std::unique_ptr<Distribution>
inverseGaussianSumRule(const std::vector<ClosureTerm> &terms)
{
  double phi = kNaN;
  double total_mu = 0.0;
  for (const auto &t : terms) {
    if (!t.dist) return nullptr;                    /* additive constant */
    if (t.a != 1.0 || t.b != 0.0) return nullptr;   /* scaled / shifted */
    const double mu = t.dist->p1(), lambda = t.dist->p2();
    if (!(mu > 0.0) || !(lambda > 0.0)) return nullptr;
    const double w_phi = lambda / (mu * mu);
    if (std::isnan(phi))    phi = w_phi;
    else if (phi != w_phi)  return nullptr;         /* different ratio */
    total_mu += mu;
  }
  if (!(total_mu > 0.0) || std::isnan(phi)) return nullptr;
  return std::make_unique<InverseGaussianDistribution>(
    total_mu, phi * total_mu * total_mu);
}

[[maybe_unused]] const ClosureRuleRegistrar inverse_gaussian_sum_rule(
  "inverse_gaussian", "inverse_gaussian", &inverseGaussianSumRule);

[[maybe_unused]] const DistributionFamilyRegistrar inverse_gaussian_family_registrar(
  inverse_gaussian_family);

}  // namespace

}  // namespace provsql
