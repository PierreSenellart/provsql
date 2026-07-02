/**
 * @file uniform.cpp
 * @brief Uniform on [a, b] family implementation.
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

/**
 * @brief Raw moments of @c X ~ Uniform(p1, p2) truncated to @c [a, b].
 *
 * The intersection @c [a', b'] = [max(p1,a), min(p2,b)] is uniform;
 * its k-th raw moment is @c (b'^{k+1} - a'^{k+1}) / ((k+1)(b' - a')).
 */
double truncated_uniform_raw_moment(double p1, double p2, double a, double b,
                                    unsigned k)
{
  const double lo = std::max(p1, a);
  const double hi = std::min(p2, b);
  if (hi <= lo) return kNaN;
  if (k == 0) return 1.0;
  return (std::pow(hi, static_cast<double>(k + 1))
        - std::pow(lo, static_cast<double>(k + 1)))
       / ((k + 1) * (hi - lo));
}

/** @brief Uniform on [a=p1, b=p2]. */
class UniformDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  DistKind kind() const override { return DistKind::Uniform; }
  double mean() const override { return 0.5 * (p1_ + p2_); }
  double variance() const override {
    const double w = p2_ - p1_;
    return (w * w) / 12.0;
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    const double a = p1_, b = p2_, kp1 = static_cast<double>(k + 1);
    return (std::pow(b, kp1) - std::pow(a, kp1)) / (kp1 * (b - a));
  }
  double pdf(double c) const override {
    if (!(p2_ > p1_)) return kNaN;
    return (c < p1_ || c > p2_) ? 0.0 : 1.0 / (p2_ - p1_);
  }
  double cdf(double c) const override {
    if (c <= p1_) return 0.0;
    if (c >= p2_) return 1.0;
    return (c - p1_) / (p2_ - p1_);
  }
  DistSupport support() const override { return {p1_, p2_}; }
  bool integrationRange(double &lo, double &hi) const override {
    lo = p1_;
    hi = p2_;
    return hi > lo;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    const double pad = 0.15 * (p2_ - p1_);
    lo = std::isfinite(lo) ? std::max(lo, p1_ - pad) : p1_ - pad;
    hi = std::isfinite(hi) ? std::min(hi, p2_ + pad) : p2_ + pad;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::uniform_real_distribution<double> d(p1_, p2_);
    return d(rng);
  }
  std::optional<double> quantile(double p) const override {
    if (!(p2_ > p1_)) return std::nullopt;
    return p1_ + p * (p2_ - p1_);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned k) const override {
    const double r = truncated_uniform_raw_moment(p1_, p2_, lo, hi, k);
    if (std::isnan(r)) return std::nullopt;
    return r;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    /* The caller (matchTruncatedSingleRv) already intersected the
     * event's interval with the RV's natural [a, b] support, so a plain
     * uniform draw on [lo, hi] is the conditional distribution. */
    std::uniform_real_distribution<double> U(lo, hi);
    std::vector<double> out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) out.push_back(U(rng));
    return out;
  }
  std::optional<double> iidOrderStatMean(std::size_t n,
                                         bool isMax) const override {
    const double a = p1_, b = p2_;
    const double frac = isMax ? static_cast<double>(n) / (n + 1)
                              : 1.0 / (n + 1);
    return a + (b - a) * frac;
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    if (a == 0.0) return nullptr;
    /* A negative coefficient flips the bounds. */
    double lo = (a > 0.0) ? a * p1_ : a * p2_;
    double hi = (a > 0.0) ? a * p2_ : a * p1_;
    if (b != 0.0) { lo += b; hi += b; }
    return std::make_unique<UniformDistribution>(lo, hi);
  }
  std::string serialise() const override {
    return "uniform:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

/* (Uniform, +, Uniform): NOT closed for two distinct uniforms (the sum
 * has a triangular / trapezoidal density), which the rule expresses by
 * declining a second Uniform term.  A single (possibly scaled / negated)
 * Uniform plus any number of constants is just the affine transform
 * a·U + Σb, with a negative coefficient flipping the bounds. */
std::unique_ptr<Distribution>
uniformSumRule(const std::vector<ClosureTerm> &terms)
{
  const ClosureTerm *uniform = nullptr;
  for (const auto &t : terms) {
    if (!t.dist) continue;
    if (uniform) return nullptr;   /* second Uniform term */
    uniform = &t;
  }
  /* Dispatch guarantees at least one RV term.  Every wire's additive
   * offset folds into the global one: (a·U + b_term) + offsets =
   * a·U + (b_term + offsets). */
  double b_total = 0.0;
  for (const auto &t : terms) b_total += t.b;
  const double a  = uniform->a;
  const double p1 = uniform->dist->p1();
  const double p2 = uniform->dist->p2();
  const double new_lo = (a > 0.0) ? a * p1 + b_total : a * p2 + b_total;
  const double new_hi = (a > 0.0) ? a * p2 + b_total : a * p1 + b_total;
  return std::make_unique<UniformDistribution>(new_lo, new_hi);
}

[[maybe_unused]] const ClosureRuleRegistrar uniform_sum_rule(
  DistKind::Uniform, DistKind::Uniform, &uniformSumRule);

/* ∫_c^d F_X(y) dy for X ~ Uniform(a, b) (b > a), i.e. the integral of the
 * clamped ramp clamp((y-a)/(b-a), 0, 1) over [c, d].  Used by the
 * Uniform-Uniform comparison closed form P(X < Y) = E_Y[F_X(Y)]. */
double integralUniformCdf(double a, double b, double c, double d)
{
  double total = 0.0;
  /* y in [a, b]: integrand (y-a)/(b-a) */
  const double lo = std::max(c, a), hi = std::min(d, b);
  if (hi > lo)
    total += ((hi - a) * (hi - a) - (lo - a) * (lo - a)) / (2.0 * (b - a));
  /* y >= b: integrand 1 */
  const double lo2 = std::max(c, b);
  if (d > lo2)
    total += d - lo2;
  /* y <= a: integrand 0 (contributes nothing) */
  return total;
}

/* P(X < Y) = E_Y[F_X(Y)] = (1/(d-c)) ∫_c^d F_X(y) dy, geometric. */
double uniformPairLess(const Distribution &X, const Distribution &Y)
{
  const double a = X.p1(), b = X.p2(), c = Y.p1(), d = Y.p2();
  if (!(b > a && d > c)) return kNaN;
  return integralUniformCdf(a, b, c, d) / (d - c);
}

[[maybe_unused]] const ComparatorRuleRegistrar uniform_less_rule(
  DistKind::Uniform, DistKind::Uniform, &uniformPairLess);

[[maybe_unused]] const DistributionFamilyRegistrar uniform_family(
  DistKind::Uniform, "uniform", 2,
  [](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<UniformDistribution>(p1, p2);
  });

}  // namespace

}  // namespace provsql
