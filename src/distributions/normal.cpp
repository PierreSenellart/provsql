/**
 * @file normal.cpp
 * @brief Normal(μ, σ) family implementation.
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

// (j-1)!! with the empty-product convention (-1)!! = 1.
double double_factorial_minus_one(unsigned j)
{
  if (j == 0) return 1.0;
  double r = 1.0;
  for (unsigned i = 1; i < j; i += 2) r *= static_cast<double>(i);
  return r;
}

/// Standard normal pdf φ(z) = exp(-z²/2)/√(2π).
double phi(double z)
{
  static const double INV_SQRT_2PI = 1.0 / std::sqrt(2.0 * M_PI);
  return INV_SQRT_2PI * std::exp(-0.5 * z * z);
}

/// Standard normal CDF Φ(z) = ½(1 + erf(z/√2)).  Mirrors the
/// NormalDistribution::cdf convention so the truncation formulas
/// here use the same numerics.
double Phi(double z)
{
  static const double SQRT2 = std::sqrt(2.0);
  return 0.5 * (1.0 + std::erf(z / SQRT2));
}

/**
 * @brief Inverse standard-normal CDF, Beasley-Springer-Moro (1995).
 *
 * Returns @c z such that @f$\Phi(z) = p@f$.  Accurate to about
 * @c 1e-7 over @c p ∈ [0.02425, 1 - 0.02425], with a tail
 * rational fallback for the rest of @c (0, 1).  Callers must clamp
 * @c p strictly inside @c (0, 1) since the function diverges at the
 * endpoints; the truncated-normal sampler clamps to
 * @c [1e-15, 1 - 1e-15] before each call.
 *
 * The Beasley-Springer-Moro routine is in widespread library use
 * (NumPy/SciPy 'norminv', etc.) and its accuracy is several orders of
 * magnitude tighter than the sampling noise the tests can detect at
 * 10k draws, so it's a comfortable margin.
 */
double inv_phi(double p)
{
  static const double a[] = {
    -3.969683028665376e+01,  2.209460984245205e+02,
    -2.759285104469687e+02,  1.383577518672690e+02,
    -3.066479806614716e+01,  2.506628277459239e+00
  };
  static const double b[] = {
    -5.447609879822406e+01,  1.615858368580409e+02,
    -1.556989798598866e+02,  6.680131188771972e+01,
    -1.328068155288572e+01
  };
  static const double c_arr[] = {
    -7.784894002430293e-03, -3.223964580411365e-01,
    -2.400758277161838e+00, -2.549732539343734e+00,
     4.374664141464968e+00,  2.938163982698783e+00
  };
  static const double d[] = {
     7.784695709041462e-03,  3.224671290700398e-01,
     2.445134137142996e+00,  3.754408661907416e+00
  };
  static const double p_low  = 0.02425;
  static const double p_high = 1.0 - p_low;

  if (p < p_low) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
              + c_arr[3])*q + c_arr[4])*q + c_arr[5])
         / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
  }
  if (p <= p_high) {
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5]) * q
         / (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - p));
  return -(((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
             + c_arr[3])*q + c_arr[4])*q + c_arr[5])
        / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
}

/**
 * @brief Raw moments of @c X ~ Normal(μ, σ) truncated to @c [a, b].
 *
 * Closed form via the integration-by-parts recurrence on the
 * standardised variable Z = (X - μ)/σ:
 *   E[Z^{k}|α<Z<β] = (k-1) E[Z^{k-2}|α<Z<β]
 *                  + (α^{k-1}φ(α) − β^{k-1}φ(β)) / (Φ(β) − Φ(α))
 * with E[Z^0|…] = 1 and E[Z^1|…] = (φ(α) − φ(β)) / (Φ(β) − Φ(α))
 * (Greene, "Econometric Analysis", 5e, App. F).  Then expand
 * E[X^k] = E[(μ + σZ)^k] binomially.
 *
 * @c α = -∞ corresponds to @p a = -INFINITY (semi-infinite left tail);
 * @c β = +∞ to @p b = +INFINITY.  Returns @c NaN if @c P(α<Z<β) is
 * below a numerical floor (so the caller falls through to MC).
 */
double truncated_normal_raw_moment(double mu, double sigma, double a, double b,
                                   unsigned k)
{
  const double alpha = std::isfinite(a) ? (a - mu) / sigma : -kInf;
  const double beta  = std::isfinite(b) ? (b - mu) / sigma : +kInf;
  const double Phi_alpha = std::isfinite(alpha) ? Phi(alpha) : 0.0;
  const double Phi_beta  = std::isfinite(beta)  ? Phi(beta)  : 1.0;
  const double Z = Phi_beta - Phi_alpha;
  if (Z < 1e-12) return kNaN;

  const double phi_alpha = std::isfinite(alpha) ? phi(alpha) : 0.0;
  const double phi_beta  = std::isfinite(beta)  ? phi(beta)  : 0.0;

  /* E[Z^k | α<Z<β] via recurrence; store all moments up to k. */
  std::vector<double> M(k + 1, 0.0);
  M[0] = 1.0;
  if (k >= 1) M[1] = (phi_alpha - phi_beta) / Z;
  for (unsigned m = 2; m <= k; ++m) {
    /* α^{m-1}·φ(α) and β^{m-1}·φ(β); take 0 when the endpoint is
     * infinite (the φ factor vanishes faster than any polynomial). */
    double end_term = 0.0;
    if (std::isfinite(alpha))
      end_term += std::pow(alpha, static_cast<double>(m - 1)) * phi_alpha;
    if (std::isfinite(beta))
      end_term -= std::pow(beta, static_cast<double>(m - 1)) * phi_beta;
    M[m] = (m - 1) * M[m - 2] + end_term / Z;
  }

  /* E[X^k] = E[(μ + σZ)^k] = Σ_{i=0..k} C(k,i) μ^{k-i} σ^i E[Z^i|…]. */
  double total = 0.0;
  for (unsigned i = 0; i <= k; ++i) {
    total += binomial_coeff(k, i)
           * std::pow(mu, static_cast<double>(k - i))
           * std::pow(sigma, static_cast<double>(i))
           * M[i];
  }
  return total;
}

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
  std::optional<double> quantile(double p) const override {
    if (!(p2_ > 0.0)) return std::nullopt;
    return p1_ + p2_ * inv_phi(p);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned k) const override {
    const double r = truncated_normal_raw_moment(p1_, p2_, lo, hi, k);
    if (std::isnan(r)) return std::nullopt;
    return r;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    const double mu = p1_, sigma = p2_;
    if (!(sigma > 0.0)) return std::nullopt;
    const double sqrt2 = std::sqrt(2.0);
    const double alpha = (lo - mu) / sigma;
    const double beta  = (hi - mu) / sigma;
    const double Phi_a = std::isfinite(alpha)
      ? 0.5 * (1.0 + std::erf(alpha / sqrt2))
      : (alpha < 0 ? 0.0 : 1.0);
    const double Phi_b = std::isfinite(beta)
      ? 0.5 * (1.0 + std::erf(beta / sqrt2))
      : (beta < 0 ? 0.0 : 1.0);
    if (!(Phi_a < Phi_b)) return std::nullopt;
    /* Inverse-CDF transform.  Clamp the target probability strictly
     * inside (0, 1) so @c inv_phi does not diverge near the asymptotes.
     * The 1e-15 margin is well below the BSM approximation's intrinsic
     * accuracy floor (~1e-7), so it's a safe sentinel. */
    static constexpr double EPS = 1e-15;
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    std::vector<double> out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
      double u = Phi_a + U01(rng) * (Phi_b - Phi_a);
      if (u < EPS)        u = EPS;
      if (u > 1.0 - EPS)  u = 1.0 - EPS;
      const double z = inv_phi(u);
      out.push_back(mu + sigma * z);
    }
    return out;
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

[[maybe_unused]] const DistributionFamilyRegistrar normal_family(
  DistKind::Normal, "normal", 2,
  [](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<NormalDistribution>(p1, p2);
  });

}  // namespace

}  // namespace provsql
