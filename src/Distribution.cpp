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

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

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
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p2_ > 0.0)) return false;
    lo = p1_ - 12.0 * p2_;
    hi = p1_ + 12.0 * p2_;
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = p1_ - 4.0 * p2_;
    if (!std::isfinite(hi)) hi = p1_ + 4.0 * p2_;
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    std::normal_distribution<double> d(p1_, p2_);
    return d(rng);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    if (a == 0.0) return nullptr;
    double mu = a * p1_;
    if (b != 0.0) mu += b;
    return std::make_unique<NormalDistribution>(mu, std::fabs(a) * p2_);
  }
  std::string serialise() const override {
    return "normal:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
  std::optional<double> asDirac() const override {
    if (p2_ == 0.0) return p1_;
    return std::nullopt;
  }
};

/* (Normal, +, Normal): every term a·Zᵢ + bᵢ over independent normals
 * sums to Normal(Σ(aᵢμᵢ + bᵢ), √(Σ aᵢ²σᵢ²)).  A degenerate total
 * variance would make the closure a Dirac at the total mean; decline and
 * leave that to other passes (in practice unreachable: σ=0 normals are
 * constructed as gate_value by provsql.normal, so total_var > 0 whenever
 * the closure fires). */
std::unique_ptr<Distribution>
normalSumRule(const std::vector<ClosureTerm> &terms)
{
  double total_mean = 0.0;
  double total_var  = 0.0;
  for (const auto &t : terms) {
    total_mean += t.b;
    if (!t.dist) continue;
    const double mu    = t.dist->p1();
    const double sigma = t.dist->p2();
    total_mean += t.a * mu;
    total_var  += t.a * t.a * sigma * sigma;
  }
  if (total_var <= 0.0) return nullptr;
  return std::make_unique<NormalDistribution>(total_mean,
                                              std::sqrt(total_var));
}

[[maybe_unused]] const ClosureRuleRegistrar normal_sum_rule(
  DistKind::Normal, DistKind::Normal, &normalSumRule);

/* P(X < Y) for X, Y independent normals.  Reduces to P(X - Y < 0) with
 * X - Y ~ N(μ_X - μ_Y, √(σ_X² + σ_Y²)). */
double normalPairLess(const Distribution &X, const Distribution &Y)
{
  return NormalDistribution(
    X.p1() - Y.p1(),
    std::sqrt(X.p2() * X.p2() + Y.p2() * Y.p2())).cdf(0.0);
}

[[maybe_unused]] const ComparatorRuleRegistrar normal_less_rule(
  DistKind::Normal, DistKind::Normal, &normalPairLess);

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

/* Function-local statics so registration (dynamic initialisation of the
 * per-family registrar objects) never observes an uninitialised map,
 * whatever the TU initialisation order. */
std::map<std::pair<DistKind, DistKind>, ComparatorRule> &comparatorRules()
{
  static std::map<std::pair<DistKind, DistKind>, ComparatorRule> rules;
  return rules;
}

std::map<std::pair<DistKind, DistKind>, ClosureRule> &closureRules()
{
  static std::map<std::pair<DistKind, DistKind>, ClosureRule> rules;
  return rules;
}

/* Registry-miss default: P(X < Y) by the 1-D quadrature
 * P(X<Y) = ∫ (1 - F_Y(t)) f_X(t) dt over X's integration range (composite
 * Simpson).  Family-agnostic -- needs only pdf / cdf / integrationRange.
 * NaN when a density / CDF is undefined (e.g. a non-integer Erlang shape),
 * so the caller falls back to Monte Carlo. */
double quadraturePairLess(const Distribution &X, const Distribution &Y)
{
  double lo, hi;
  if (!X.integrationRange(lo, hi))
    return kNaN;
  const int N = 4000;
  const double h = (hi - lo) / N;
  double acc = 0.0;
  for (int i = 0; i <= N; ++i) {
    const double t = lo + i * h;
    const double fX = X.pdf(t);
    const double FY = Y.cdf(t);
    if (std::isnan(fX) || std::isnan(FY))
      return kNaN;
    const double coeff = (i == 0 || i == N) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
    acc += coeff * (1.0 - FY) * fX;
  }
  return acc * h / 3.0;
}

}  // namespace

void registerComparatorRule(DistKind x, DistKind y, ComparatorRule rule)
{
  comparatorRules()[{x, y}] = rule;
}

void registerClosureRule(DistKind x, DistKind y, ClosureRule rule)
{
  closureRules()[{x, y}] = rule;
}

std::unique_ptr<Distribution> closePlusTerms(
  const std::vector<ClosureTerm> &terms)
{
  const auto &rules = closureRules();
  ClosureRule rule = nullptr;
  bool first = true;
  DistKind k0{};
  for (const auto &t : terms) {
    if (!t.dist) continue;
    if (first) {
      first = false;
      k0 = t.dist->kind();
      const auto it = rules.find({k0, k0});
      if (it == rules.end()) return nullptr;
      rule = it->second;
    } else {
      const auto it = rules.find({k0, t.dist->kind()});
      if (it == rules.end() || it->second != rule) return nullptr;
    }
  }
  if (!rule) return nullptr;   /* no RV term: the constant fold's job */
  return rule(terms);
}

double comparatorPairLess(const Distribution &X, const Distribution &Y)
{
  const auto &rules = comparatorRules();
  const auto it = rules.find({X.kind(), Y.kind()});
  double pLess = kNaN;
  if (it != rules.end())
    pLess = it->second(X, Y);
  /* Registry miss, or a rule that declined (parameter guard, or a shape
   * outside its closed form): generic quadrature. */
  if (std::isnan(pLess))
    pLess = quadraturePairLess(X, Y);
  return pLess;
}

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
