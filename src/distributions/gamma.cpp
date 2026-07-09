/**
 * @file gamma.cpp
 * @brief Gamma(k, λ) family implementation (shape k any positive real,
 *        rate λ).
 *
 * The general-shape counterpart of Erlang: the SQL constructor routes
 * integer shapes through @c provsql.erlang, so a @c gamma leaf always
 * carries a non-integer (or sub-1) shape here.  The CDF is the
 * regularised lower incomplete gamma @f$P(k, \lambda x)@f$ (series /
 * continued fraction); Chi-squared is the SQL-level sugar
 * <tt>gamma(k/2, 1/2)</tt>.
 *
 * Self-contained family implementation: the class is file-local and
 * reaches the evaluators only through the registrars at the bottom
 * (DistributionRegistry factory + closure rule).  First family added
 * under the per-family layout -- no evaluator or parser code changes.
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

/** @brief Gamma(k=p1 shape > 0, λ=p2 rate > 0). */
class GammaDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override { return p1_ / p2_; }
  double variance() const override { return p1_ / (p2_ * p2_); }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    // Γ(s+k)/(Γ(s) λ^k) = s(s+1)...(s+k-1)/λ^k, valid for any real s > 0.
    const double s = p1_;
    double rising = 1.0;
    for (unsigned i = 0; i < k; ++i) rising *= (s + static_cast<double>(i));
    return rising / std::pow(p2_, static_cast<double>(k));
  }
  double pdf(double c) const override {
    const double s = p1_, lambda = p2_;
    if (!(s > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c < 0.0) return 0.0;
    if (c == 0.0) {
      /* The density diverges at 0 for shape < 1 (integrable
       * singularity).  Report NaN rather than +∞ so the Simpson
       * quadratures -- which cannot resolve a singular endpoint on a
       * uniform grid -- decline and fall back to Monte Carlo instead
       * of integrating garbage. */
      if (s < 1.0) return kNaN;
      return (s == 1.0) ? lambda : 0.0;
    }
    return std::exp(s * std::log(lambda) + (s - 1.0) * std::log(c)
                    - lambda * c - std::lgamma(s));
  }
  double cdf(double c) const override {
    const double s = p1_, lambda = p2_;
    if (!(s > 0.0) || !(lambda > 0.0)) return kNaN;
    if (c <= 0.0) return 0.0;
    return gammaP(s, lambda * c);
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0 && p2_ > 0.0)) return false;
    /* Same k + 12√k window as Erlang (mean + 12 standard deviations
     * in units of 1/λ). */
    lo = 0.0;
    hi = (p1_ + 12.0 * std::sqrt(p1_)) / p2_;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = std::max(2.0 * p1_ / p2_, 6.0 / p2_);
    /* Shape < 1: the density has an integrable singularity at 0 (pdf(0)
     * is NaN by design, so the quadratures decline).  Nudge the window's
     * left edge just inside so a uniform plot grid never samples the
     * singular endpoint -- otherwise every curve payload for a sub-1
     * shape would be all-NaN-poisoned and dropped. */
    if (p1_ < 1.0 && lo <= 0.0) lo = 0.005 * hi;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    // Gamma(shape=k, scale=1/λ) in the std parameterisation.
    std::gamma_distribution<double> d(p1_, 1.0 / p2_);
    return d(rng);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* Same positive-scaling-only closure as Erlang / Exponential. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    if (!(p1_ > 0.0)) return nullptr;
    return std::make_unique<GammaDistribution>(p1_, p2_ / a);
  }
  std::string serialise() const override {
    return "gamma:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

const DistributionFamily gamma_family = {
  "gamma", 2, "Γ", {"k", "λ"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<GammaDistribution>(p1, p2);
  }};

const DistributionFamily &GammaDistribution::family() const
{
  return gamma_family;
}

/* (Gamma, +, Gamma), same rate: Gamma(Σkᵢ, λ) for independent terms.
 * Strict shape like the Erlang rule: unscaled, unshifted, no additive
 * constants, rates exactly equal.  Mixed Gamma + Exponential / Erlang
 * sums are NOT registered (the closure-rule dispatch requires one rule
 * per family combination and those pairs belong to the Erlang-sum
 * rule); they stay unfolded and Monte Carlo handles them, which is
 * sound -- only the closed form is missed. */
std::unique_ptr<Distribution>
gammaSumRule(const std::vector<ClosureTerm> &terms)
{
  double lambda = kNaN;
  double total_shape = 0.0;
  for (const auto &t : terms) {
    if (!t.dist) return nullptr;                    /* additive constant */
    if (t.a != 1.0 || t.b != 0.0) return nullptr;   /* scaled / shifted */
    if (!(t.dist->p1() > 0.0)) return nullptr;
    const double w_lambda = t.dist->p2();
    if (std::isnan(lambda))      lambda = w_lambda;
    else if (lambda != w_lambda) return nullptr;    /* different rate */
    total_shape += t.dist->p1();
  }
  return std::make_unique<GammaDistribution>(total_shape, lambda);
}

[[maybe_unused]] const ClosureRuleRegistrar gamma_sum_rule(
  "gamma", "gamma", &gammaSumRule);

/* Gamma-Gamma conjugacy: a Gamma(k₀, θ) observation with the latent in
 * the RATE slot (k₀ a known literal shape) updates a Gamma(k, λ) prior
 * to Gamma(k+k₀, λ+d).  The predictive is the compound-gamma
 * (scaled beta-prime) density
 * m(d) = Γ(k+k₀)/(Γ(k)Γ(k₀)) · λ^k d^{k₀−1} / (λ+d)^{k+k₀}. */
bool gammaRateConjugateUpdate(double &k, double &lambda,
                              const DistributionTemplate &lik, double d)
{
  const double k0 = lik.p1.literal;
  if (!(k > 0.0) || !(lambda > 0.0) || !(k0 > 0.0) || !(d > 0.0))
    return false;
  k += k0;
  lambda += d;
  return true;
}

double gammaRateLogPredictive(double k, double lambda,
                              const DistributionTemplate &lik, double d)
{
  const double k0 = lik.p1.literal;
  if (!(k > 0.0) || !(lambda > 0.0) || !(k0 > 0.0) || !(d > 0.0))
    return kNaN;
  return std::lgamma(k + k0) - std::lgamma(k) - std::lgamma(k0)
       + k * std::log(lambda) + (k0 - 1.0) * std::log(d)
       - (k + k0) * std::log(lambda + d);
}

[[maybe_unused]] const ConjugateRuleRegistrar gamma_rate_conjugate(
  "gamma", 1, "gamma",
  {&gammaRateConjugateUpdate, &gammaRateLogPredictive});

[[maybe_unused]] const DistributionFamilyRegistrar gamma_family_registrar(
  gamma_family);

}  // namespace

}  // namespace provsql
