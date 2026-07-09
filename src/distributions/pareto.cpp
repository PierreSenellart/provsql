/**
 * @file pareto.cpp
 * @brief Pareto(xₘ, α) family implementation: scale (minimum)
 *        @c xₘ > 0, shape @c α > 0.
 *
 * The canonical heavy-tailed power law (wealth, city / file sizes,
 * insurance large losses).  Raw moments are @b infinite for
 * @c α <= @c j and reported as such (+Infinity, honestly) rather than
 * estimated; the tail's self-similarity (X | X > a is
 * Pareto(max(a, xₘ), α)) gives exact truncated moments and a
 * rejection-free conditional sampler, and ln(X/xₘ) being Exp(α) gives
 * a full cross-parameter comparator closed form -- important because
 * the tail is exactly where a uniform quadrature grid fails.
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

/** @brief Pareto(xₘ=p1 scale > 0, α=p2 shape > 0). */
class ParetoDistribution final : public BaseDistribution {
public:
  using BaseDistribution::BaseDistribution;
  const DistributionFamily &family() const override;
  double mean() const override {
    if (!(p2_ > 1.0)) return kInf;   /* diverges for α <= 1 */
    return p2_ * p1_ / (p2_ - 1.0);
  }
  double variance() const override {
    if (!(p2_ > 2.0)) return kInf;   /* diverges for α <= 2 */
    const double am1 = p2_ - 1.0;
    return p1_ * p1_ * p2_ / (am1 * am1 * (p2_ - 2.0));
  }
  double rawMoment(unsigned k) const override {
    if (k == 0) return 1.0;
    const double j = static_cast<double>(k);
    if (!(p2_ > j)) return kInf;     /* E[X^j] diverges for α <= j */
    return p2_ * std::pow(p1_, j) / (p2_ - j);
  }
  double pdf(double c) const override {
    const double xm = p1_, alpha = p2_;
    if (!(xm > 0.0) || !(alpha > 0.0)) return kNaN;
    if (c < xm) return 0.0;
    return (alpha / xm) * std::pow(xm / c, alpha + 1.0);
  }
  double cdf(double c) const override {
    const double xm = p1_, alpha = p2_;
    if (!(xm > 0.0) || !(alpha > 0.0)) return kNaN;
    if (c <= xm) return 0.0;
    return 1.0 - std::pow(xm / c, alpha);
  }
  DistSupport support() const override { return {p1_, kInf}; }
  bool integrationRange(double &lo, double &hi) const override {
    if (!(p1_ > 0.0 && p2_ > 0.0)) return false;
    /* quantile(1 - 1e-9).  The power-law tail means the window is
     * xₘ·10^{9/α} -- huge for small α -- so a uniform grid resolves it
     * poorly; the full cross-parameter comparator closed form below
     * keeps Pareto-vs-Pareto off that path. */
    lo = p1_;
    hi = p1_ * std::pow(1e9, 1.0 / p2_);
    return true;
  }
  std::pair<double, double> plotRange(double trunc_lo, double trunc_hi) const override {
    double lo = trunc_lo, hi = trunc_hi;
    if (!std::isfinite(lo)) lo = p1_;
    if (!std::isfinite(hi))
      hi = p1_ * std::pow(1e3, 1.0 / p2_);   /* q(0.999) */
    return {lo, hi};
  }
  double sample(std::mt19937_64 &rng) const override {
    /* Inverse CDF of a uniform draw (no std::pareto_distribution). */
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    return p1_ * std::pow(1.0 - U01(rng), -1.0 / p2_);
  }
  std::optional<double> quantile(double p) const override {
    if (!(p1_ > 0.0) || !(p2_ > 0.0)) return std::nullopt;
    return p1_ * std::pow(1.0 - p, -1.0 / p2_);
  }
  std::optional<double> truncatedRawMoment(double lo, double hi,
                                           unsigned j) const override {
    const double xm = p1_, alpha = p2_;
    if (!(xm > 0.0) || !(alpha > 0.0)) return std::nullopt;
    const double a = std::max(std::isfinite(lo) ? lo : xm, xm);
    if (std::isfinite(hi) && hi <= a) return std::nullopt;
    const double mass = std::pow(xm / a, alpha)
      - (std::isfinite(hi) ? std::pow(xm / hi, alpha) : 0.0);
    if (mass < 1e-12) return std::nullopt;
    if (j == 0) return 1.0;
    const double jd = static_cast<double>(j);
    if (jd == alpha) return std::nullopt;   /* logarithmic edge case */
    if (!std::isfinite(hi) && !(alpha > jd))
      return kInf;                          /* honestly divergent */
    /* ∫_a^b x^j α xₘ^α x^{-α-1} dx
     *   = α xₘ^α (b^{j-α} - a^{j-α}) / (j - α). */
    const double upper =
      std::isfinite(hi) ? std::pow(hi, jd - alpha) : 0.0;
    return alpha * std::pow(xm, alpha)
         * (upper - std::pow(a, jd - alpha)) / (jd - alpha) / mass;
  }
  std::optional<std::vector<double>> sampleTruncated(
    std::mt19937_64 &rng, double lo, double hi, unsigned n) const override {
    const double xm = p1_, alpha = p2_;
    if (!(xm > 0.0) || !(alpha > 0.0)) return std::nullopt;
    const double a = std::max(std::isfinite(lo) ? lo : xm, xm);
    std::vector<double> out;
    out.reserve(n);
    std::uniform_real_distribution<double> U01(0.0, 1.0);
    if (std::isinf(hi)) {
      /* Self-similarity: X | X > a is Pareto(a, α). */
      for (unsigned i = 0; i < n; ++i)
        out.push_back(a * std::pow(1.0 - U01(rng), -1.0 / alpha));
      return out;
    }
    if (hi <= a) return std::nullopt;
    /* Two-sided: exact inverse CDF on [F(a), F(hi)]. */
    const double F_a = 1.0 - std::pow(xm / a, alpha);
    const double F_b = 1.0 - std::pow(xm / hi, alpha);
    if (!(F_a < F_b)) return std::nullopt;
    for (unsigned i = 0; i < n; ++i) {
      const double u = F_a + U01(rng) * (F_b - F_a);
      out.push_back(xm * std::pow(1.0 - u, -1.0 / alpha));
    }
    return out;
  }
  std::optional<double> iidOrderStatMean(std::size_t n,
                                         bool isMax) const override {
    if (!(p1_ > 0.0) || !(p2_ > 0.0)) return std::nullopt;
    if (isMax) return std::nullopt;   /* no elementary max closed form */
    /* Min-stability: the min of n i.i.d. Pareto(xₘ, α) is
     * Pareto(xₘ, nα). */
    const double na = static_cast<double>(n) * p2_;
    if (!(na > 1.0)) return kInf;
    return na * p1_ / (na - 1.0);
  }
  std::unique_ptr<Distribution> affine(double a, double b) const override {
    /* Positive scalings rescale xₘ; negations / offsets leave the
     * family. */
    if (!(a > 0.0) || b != 0.0) return nullptr;
    return std::make_unique<ParetoDistribution>(a * p1_, p2_);
  }
  std::string serialise() const override {
    return "pareto:" + double_to_text(p1_) + "," + double_to_text(p2_);
  }
};

const DistributionFamily pareto_family = {
  "pareto", 2, "Par", {"xₘ", "α"},
  +[](double p1, double p2) -> std::unique_ptr<Distribution> {
    return std::make_unique<ParetoDistribution>(p1, p2);
  }};

const DistributionFamily &ParetoDistribution::family() const
{
  return pareto_family;
}

/* P(X < Y) for independent Paretos, exact for ANY parameters (this is
 * the pair whose heavy tails the generic quadrature handles worst).
 * With xₘX <= xₘY:
 *   P(X < Y) = 1 - (α_Y / (α_X + α_Y)) · (xₘX / xₘY)^{α_X}
 * (integrate F_X over Y's density; same-scale reduces to the
 * exponential ratio α_X/(α_X+α_Y) through ln(X/xₘ) ~ Exp(α));
 * the xₘX > xₘY case is the mirror complement. */
double paretoPairLess(const Distribution &X, const Distribution &Y)
{
  const double xmx = X.p1(), ax = X.p2();
  const double xmy = Y.p1(), ay = Y.p2();
  if (!(xmx > 0.0) || !(ax > 0.0) || !(xmy > 0.0) || !(ay > 0.0))
    return kNaN;
  if (xmx <= xmy)
    return 1.0 - (ay / (ax + ay)) * std::pow(xmx / xmy, ax);
  return (ax / (ax + ay)) * std::pow(xmy / xmx, ay);
}

[[maybe_unused]] const ComparatorRuleRegistrar pareto_less_rule(
  "pareto", "pareto", &paretoPairLess);

/* Gamma-Pareto conjugacy: a Pareto(xₘ₀, θ) observation with the latent
 * in the SHAPE slot (xₘ₀ a known literal scale) updates a Gamma(k, λ)
 * prior to Gamma(k+1, λ + ln(d/xₘ₀)) -- the likelihood
 * f(d|α) = (α/d)·e^{−α ln(d/xₘ₀)} is a gamma kernel in α.  The
 * predictive is m(d) = (k/d) · λ^k / (λ + ln(d/xₘ₀))^{k+1}. */
bool paretoShapeConjugateUpdate(double &k, double &lambda,
                                const DistributionTemplate &lik, double d)
{
  const double xm0 = lik.p1.literal;
  if (!(k > 0.0) || !(lambda > 0.0) || !(xm0 > 0.0) || !(d >= xm0))
    return false;
  k += 1.0;
  lambda += std::log(d / xm0);
  return true;
}

double paretoShapeLogPredictive(double k, double lambda,
                                const DistributionTemplate &lik, double d)
{
  const double xm0 = lik.p1.literal;
  if (!(k > 0.0) || !(lambda > 0.0) || !(xm0 > 0.0) || !(d >= xm0))
    return kNaN;
  const double t = std::log(d / xm0);
  return std::log(k) - std::log(d) + k * std::log(lambda)
       - (k + 1.0) * std::log(lambda + t);
}

[[maybe_unused]] const ConjugateRuleRegistrar pareto_shape_conjugate(
  "pareto", 1, "gamma",
  {&paretoShapeConjugateUpdate, &paretoShapeLogPredictive});

[[maybe_unused]] const DistributionFamilyRegistrar pareto_family_registrar(
  pareto_family);

}  // namespace

}  // namespace provsql
