/**
 * @file Distribution.cpp
 * @brief Concrete per-family @c Distribution implementations (§F.1).
 *
 * The closed forms are relocated verbatim from the pre-refactor @c DistKind
 * switches (moments from @c RandomVariable.cpp, pdf/cdf from
 * @c AnalyticEvaluator.cpp, support from @c RangeCheck.cpp, sampling from
 * @c MonteCarloSampler.cpp), so behaviour is preserved by construction and
 * the @c test/ suite is the net.  Consumers migrate onto this dispatch one
 * at a time (see @c doc/TODO/distribution-refactor.md).
 */
#include "Distribution.h"

#include <cmath>
#include <limits>

namespace provsql {

namespace {

/** @name Moment helpers (relocated from RandomVariable.cpp) */
///@{
double factorial(unsigned k)
{
  double r = 1.0;
  for (unsigned i = 2; i <= k; ++i) r *= static_cast<double>(i);
  return r;
}

double binomial_coeff(unsigned n, unsigned k)
{
  if (k > n) return 0.0;
  if (k > n - k) k = n - k;
  double r = 1.0;
  for (unsigned i = 1; i <= k; ++i) {
    r *= static_cast<double>(n - i + 1);
    r /= static_cast<double>(i);
  }
  return r;
}

// (j-1)!! with the empty-product convention (-1)!! = 1.
double double_factorial_minus_one(unsigned j)
{
  if (j == 0) return 1.0;
  double r = 1.0;
  for (unsigned i = 1; i < j; i += 2) r *= static_cast<double>(i);
  return r;
}
///@}

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

/** @brief Base holding the two parameters; subclasses add closed forms. */
class BaseDistribution : public Distribution {
public:
  BaseDistribution(double p1, double p2) : p1_(p1), p2_(p2) {}
  double p1() const override { return p1_; }
  double p2() const override { return p2_; }
protected:
  double p1_, p2_;
};

/** @brief Normal(μ=p1, σ=p2). */
class NormalDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  DistKind kind() const override { return DistKind::Normal; }
  double mean() const override { return p1_; }
  double variance() const override { return p2_ * p2_; }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    if (k == 1) return mean();
    // E[X^k] = sum_{j=0,2,...}^{k} C(k,j) mu^(k-j) sigma^j (j-1)!!
    const double mu = p1_, sigma = p2_;
    double total = 0.0;
    for (unsigned j = 0; j <= k; j += 2) {
      total += binomial_coeff(k, j)
             * std::pow(mu, static_cast<double>(k - j))
             * std::pow(sigma, static_cast<double>(j))
             * double_factorial_minus_one(j);
    }
    return total;
  }
  double pdf(double c) const override {
    if (!(p2_ > 0.0)) return kNaN;
    const double SQRT_2PI = std::sqrt(2.0 * M_PI);
    const double z = (c - p1_) / p2_;
    return std::exp(-0.5 * z * z) / (p2_ * SQRT_2PI);
  }
  double cdf(double c) const override {
    const double SQRT2 = std::sqrt(2.0);
    const double z = (c - p1_) / p2_;
    return 0.5 * (1.0 + std::erf(z / SQRT2));
  }
  DistSupport support() const override { return {-kInf, kInf}; }
  double sample(std::mt19937_64 &rng) const override {
    std::normal_distribution<double> d(p1_, p2_);
    return d(rng);
  }
};

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
  double sample(std::mt19937_64 &rng) const override {
    std::uniform_real_distribution<double> d(p1_, p2_);
    return d(rng);
  }
};

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
  double sample(std::mt19937_64 &rng) const override {
    std::exponential_distribution<double> d(p1_);
    return d(rng);
  }
};

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
  double sample(std::mt19937_64 &rng) const override {
    // Gamma(shape=k, scale=1/λ) samples Erlang(k, λ) directly.
    std::gamma_distribution<double> d(p1_, 1.0 / p2_);
    return d(rng);
  }
};

}  // namespace

std::unique_ptr<Distribution> makeDistribution(const DistributionSpec &spec)
{
  switch (spec.kind) {
    case DistKind::Normal:
      return std::make_unique<NormalDistribution>(spec.p1, spec.p2);
    case DistKind::Uniform:
      return std::make_unique<UniformDistribution>(spec.p1, spec.p2);
    case DistKind::Exponential:
      return std::make_unique<ExponentialDistribution>(spec.p1, spec.p2);
    case DistKind::Erlang:
      return std::make_unique<ErlangDistribution>(spec.p1, spec.p2);
  }
  return nullptr;
}

}  // namespace provsql
