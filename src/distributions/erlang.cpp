/**
 * @file erlang.cpp
 * @brief Erlang(k, λ) family implementation (integer shape).
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

/** @brief Erlang(k=p1 integer≥1, λ=p2). */
class ErlangDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  DistKind kind() const override { return DistKind::Erlang; }
  double mean() const override { return p1_ / p2_; }
  double variance() const override { return p1_ / (p2_ * p2_); }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    // s(s+1)...(s+k-1) / lambda^k for integer shape s.
    const double s = p1_;
    double rising = 1.0;
    for (unsigned i = 0; i < k; ++i) rising *= (s + static_cast<double>(i));
    return rising / std::pow(p2_, static_cast<double>(k));
  }
  double pdf(double c) const override {
    const double s = p1_, lambda = p2_;
    if (s < 1.0 || s != std::floor(s) || !(lambda > 0.0)) return kNaN;
    if (c < 0.0) return 0.0;
    const unsigned long k = static_cast<unsigned long>(s);
    double fact = 1.0;  // (k-1)!
    for (unsigned long i = 2; i < k; ++i) fact *= static_cast<double>(i);
    return std::pow(lambda, static_cast<double>(k))
         * std::pow(c, static_cast<double>(k - 1))
         * std::exp(-lambda * c) / fact;
  }
  double cdf(double c) const override {
    const double s = p1_, lambda = p2_;
    if (s < 1.0 || s != std::floor(s)) return kNaN;
    if (c <= 0.0) return 0.0;
    const unsigned long k = static_cast<unsigned long>(s);
    const double lc = lambda * c;
    double term = 1.0, sum = 1.0;  // 1 - e^{-λc} Σ_{n=0..k-1}(λc)^n/n!
    for (unsigned long n = 1; n < k; ++n) {
      term *= lc / static_cast<double>(n);
      sum += term;
    }
    return 1.0 - std::exp(-lc) * sum;
  }
  DistSupport support() const override { return {0.0, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ >= 1.0 && p2_ > 0.0)) return false;
    lo = 0.0;
    hi = (p1_ + 12.0 * std::sqrt(p1_)) / p2_;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = std::max(2.0 * p1_ / p2_, 6.0 / p2_);
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    // Gamma(shape=k, scale=1/λ) samples Erlang(k, λ) directly.
    std::gamma_distribution<double> d(p1_, 1.0 / p2_);
    return d(rng);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* Same positive-scaling-only closure as Exponential. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    /* Integer k stored in p1; non-integer is rejected upstream by the
     * SQL constructor, but guard defensively. */
    if (p1_ < 1.0 || p1_ != std::floor(p1_)) return nullptr;
    return std::make_unique<ErlangDistribution>(p1_, p2_ / a);
  }
  std::string serialise() const override {
    /* The integer shape prints without a decimal point / exponent
     * whatever its magnitude, matching the constructor's encoding. */
    if (p1_ >= 1.0 && p1_ == std::floor(p1_))
      return "erlang:" + std::to_string(static_cast<unsigned long>(p1_))
             + "," + double_to_text(p2_);
    return "erlang:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

/* (Exp|Erlang, +, Exp|Erlang), same rate: Erlang(Σkᵢ, λ) with
 * Exp ≡ Erlang(1, λ).  Strict shape: unscaled, unshifted, no additive
 * constants (any of those leaves the family; hypoexponential /
 * different-rate sums are the sampler's job), rates exactly equal. */
std::unique_ptr<Distribution>
erlangSumRule(const std::vector<ClosureTerm> &terms)
{
  double lambda = kNaN;
  unsigned long total_shape = 0;
  for (const auto &t : terms) {
    if (!t.dist) return nullptr;                    /* additive constant */
    if (t.a != 1.0 || t.b != 0.0) return nullptr;   /* scaled / shifted */
    double w_lambda;
    unsigned long w_shape;
    if (t.dist->kind() == DistKind::Exponential) {
      w_lambda = t.dist->p1();
      w_shape  = 1;
    } else {
      /* Integer k stored in p1; guard defensively so a corrupted extra
       * cannot trigger an invalid shape sum. */
      if (t.dist->p1() < 1.0 || t.dist->p1() != std::floor(t.dist->p1()))
        return nullptr;
      w_lambda = t.dist->p2();
      w_shape  = static_cast<unsigned long>(t.dist->p1());
    }
    if (std::isnan(lambda))      lambda = w_lambda;
    else if (lambda != w_lambda) return nullptr;    /* different rate */
    total_shape += w_shape;
  }
  return std::make_unique<ErlangDistribution>(
    static_cast<double>(total_shape), lambda);
}

[[maybe_unused]] const ClosureRuleRegistrar erlang_sum_rules[] = {
  {DistKind::Exponential, DistKind::Exponential, &erlangSumRule},
  {DistKind::Exponential, DistKind::Erlang,      &erlangSumRule},
  {DistKind::Erlang,      DistKind::Exponential, &erlangSumRule},
  {DistKind::Erlang,      DistKind::Erlang,      &erlangSumRule},
};

[[maybe_unused]] const DistributionFamilyRegistrar erlang_family(
  DistKind::Erlang, "erlang", 2,
  [](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<ErlangDistribution>(p1, p2);
  });

}  // namespace

}  // namespace provsql
