/**
 * @file AnalyticEvaluator.cpp
 * @brief Implementation of the closed-form CDF resolution pass.
 *        See @c AnalyticEvaluator.h for the full docstring.
 */
#include "AnalyticEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "Aggregation.h"        // ComparisonOperator + cmpOpFromOid
#include "RandomVariable.h"     // parse_distribution_spec, parseDoubleStrict, DistKind
#include "Distribution.h"       // makeDistribution -> integrationRange
extern "C" {
#include "provsql_utils.h"      // gate_type
}

namespace provsql {

/* pdfAt / cdfAt are the free-function entry points for the cold callers
 * (single-RV-vs-constant decide, curve rendering, shape mass).  The
 * per-family closed forms live in the Distribution subclasses; hot
 * quadrature loops construct the Distribution once and call pdf/cdf on it
 * directly rather than going through these per-point. */
double pdfAt(const DistributionSpec &d, double c)
{
  return makeDistribution(d)->pdf(c);
}

double cdfAt(const DistributionSpec &d, double c)
{
  return makeDistribution(d)->cdf(c);
}

namespace {

/* All four ordered comparators reduce to either F(c) or 1 - F(c)
 * (continuous: @c < and @c <= have the same probability, ditto @c >
 * and @c >=).  EQ / NE on continuous RVs are handled universally by
 * RangeCheck (P(X = c) = 0, P(X != c) = 1, sound in every semiring
 * via gate_zero / gate_one); they should never reach this function. */
double cdfDecide(const DistributionSpec &d, ComparisonOperator op, double c)
{
  double cdf_c = cdfAt(d, c);
  if (std::isnan(cdf_c)) return cdf_c;

  switch (op) {
    case ComparisonOperator::LT:
    case ComparisonOperator::LE:
      return cdf_c;
    case ComparisonOperator::GT:
    case ComparisonOperator::GE:
      return 1.0 - cdf_c;
    case ComparisonOperator::EQ:
    case ComparisonOperator::NE:
      /* Should have been handled upstream by RangeCheck; if we still
       * see one here it means RangeCheck did not run (e.g.
       * provsql.simplify_on_load is off).  Fall through to undecided
       * rather than silently make an inconsistent choice. */
      return std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}

/* Mirror @c provsql_having_detail::flip_op without taking the
 * dependency on @c having_semantics from this file.  Used to
 * normalise @c c @c cmp @c X into @c X @c flip(cmp) @c c. */
ComparisonOperator flipCmpOp(ComparisonOperator op)
{
  switch (op) {
    case ComparisonOperator::LT: return ComparisonOperator::GT;
    case ComparisonOperator::LE: return ComparisonOperator::GE;
    case ComparisonOperator::GT: return ComparisonOperator::LT;
    case ComparisonOperator::GE: return ComparisonOperator::LE;
    case ComparisonOperator::EQ: return ComparisonOperator::EQ;
    case ComparisonOperator::NE: return ComparisonOperator::NE;
  }
  return op;
}

/* X cmp Y for X, Y independent normals.  Reduces to (X - Y) cmp 0
 * with X - Y ~ N(μ_X - μ_Y, σ_X² + σ_Y²). */
double normalDiffDecide(const DistributionSpec &X,
                        const DistributionSpec &Y,
                        ComparisonOperator op)
{
  DistributionSpec diff;
  diff.kind = DistKind::Normal;
  diff.p1 = X.p1 - Y.p1;
  diff.p2 = std::sqrt(X.p2 * X.p2 + Y.p2 * Y.p2);
  return cdfDecide(diff, op, 0.0);
}

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

/* P(X < Y) for two independent RVs of possibly-different families, by the
 * 1-D quadrature P(X<Y) = ∫ (1 - F_Y(t)) f_X(t) dt over X's support
 * (composite Simpson).  NaN when a density / CDF is undefined (e.g. a
 * non-integer Erlang shape), so the caller falls back to Monte Carlo. */
double mixedPairLess(const DistributionSpec &X, const DistributionSpec &Y)
{
  // Construct the two distributions once; the Simpson loop calls pdf/cdf on
  // them per point (never re-constructing per point).
  const auto dX = makeDistribution(X);
  const auto dY = makeDistribution(Y);
  double lo, hi;
  if (!dX->integrationRange(lo, hi))
    return std::numeric_limits<double>::quiet_NaN();
  const int N = 4000;
  const double h = (hi - lo) / N;
  double acc = 0.0;
  for (int i = 0; i <= N; ++i) {
    const double t = lo + i * h;
    const double fX = dX->pdf(t);
    const double FY = dY->cdf(t);
    if (std::isnan(fX) || std::isnan(FY))
      return std::numeric_limits<double>::quiet_NaN();
    const double coeff = (i == 0 || i == N) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
    acc += coeff * (1.0 - FY) * fX;
  }
  return acc * h / 3.0;
}

/* P(X op Y) for two independent RVs: the same-family closed forms
 * (Normal-Normal difference, Exp-Exp rate ratio, Uniform-Uniform geometric),
 * else the mixed-family 1-D quadrature.  NaN if nothing applies (caller falls
 * back to Monte Carlo).  Continuous throughout, so <,<= share a value and
 * >,>= share its complement; EQ/NE are handled upstream by RangeCheck. */
double rvVsRvDecide(const DistributionSpec &X, const DistributionSpec &Y,
                    ComparisonOperator op)
{
  double pLess = std::numeric_limits<double>::quiet_NaN();  /* P(X < Y) */

  if (X.kind == DistKind::Normal && Y.kind == DistKind::Normal)
    return normalDiffDecide(X, Y, op);  /* already reduces < / > directly */

  if (X.kind == DistKind::Exponential && Y.kind == DistKind::Exponential) {
    /* P(X < Y) = λ_X / (λ_X + λ_Y) for independent exponentials. */
    const double lx = X.p1, ly = Y.p1;
    if (lx > 0.0 && ly > 0.0)
      pLess = lx / (lx + ly);
  } else if (X.kind == DistKind::Uniform && Y.kind == DistKind::Uniform) {
    /* P(X < Y) = E_Y[F_X(Y)] = (1/(d-c)) ∫_c^d F_X(y) dy, geometric. */
    const double a = X.p1, b = X.p2, c = Y.p1, d = Y.p2;
    if (b > a && d > c)
      pLess = integralUniformCdf(a, b, c, d) / (d - c);
  }

  /* Mixed independent families (or a same-family shape the closed form
   * declined): 1-D quadrature. */
  if (std::isnan(pLess))
    pLess = mixedPairLess(X, Y);

  if (std::isnan(pLess))
    return pLess;
  if (pLess < 0.0) pLess = 0.0;
  if (pLess > 1.0) pLess = 1.0;
  switch (op) {
    case ComparisonOperator::LT:
    case ComparisonOperator::LE:
      return pLess;
    case ComparisonOperator::GT:
    case ComparisonOperator::GE:
      return 1.0 - pLess;  /* P(X > Y) = 1 - P(X < Y), continuous */
    case ComparisonOperator::EQ:
    case ComparisonOperator::NE:
      return std::numeric_limits<double>::quiet_NaN();
  }
  return std::numeric_limits<double>::quiet_NaN();
}

/* Try to parse a @c gate_value's extra as a double.  Returns NaN on
 * any failure (caller treats NaN as "skip this cmp"). */
double bareValue(const GenericCircuit &gc, gate_t g)
{
  if (gc.getGateType(g) != gate_value)
    return std::numeric_limits<double>::quiet_NaN();
  try { return parseDoubleStrict(gc.getExtra(g)); }
  catch (const CircuitException &) {
    return std::numeric_limits<double>::quiet_NaN();
  }
}

/* Try to parse a @c gate_rv's distribution spec.  Returns @c
 * std::nullopt on any failure. */
std::optional<DistributionSpec>
bareRv(const GenericCircuit &gc, gate_t g)
{
  if (gc.getGateType(g) != gate_rv)
    return std::nullopt;
  return parse_distribution_spec(gc.getExtra(g));
}

/* Closed-form P(X cmp c) for a categorical-form gate_mixture X.  X's
 * wires are [key, mul_1, ..., mul_n]; each mul_i carries its
 * probability in set_prob and its outcome value in extra (parsed as
 * float8).  The probability is just the sum of π_i over mulinputs
 * whose value satisfies the predicate.
 *
 * EQ / NE are exact too in this setting (X = c iff some outcome equals
 * c with positive mass): the RangeCheck pre-pass treats EQ / NE over
 * continuous RVs as P=0 / P=1, but a categorical is discrete so we
 * decide them here.  Returns NaN if any mulinput's extra fails to
 * parse as a finite float8 -- the cmp then falls through to MC. */
double categoricalDecide(const GenericCircuit &gc, gate_t mix,
                         ComparisonOperator op, double c)
{
  const auto &wires = gc.getWires(mix);
  double p = 0.0;
  for (std::size_t i = 1; i < wires.size(); ++i) {
    double v;
    try { v = parseDoubleStrict(gc.getExtra(wires[i])); }
    catch (const CircuitException &) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    bool hit = false;
    switch (op) {
      case ComparisonOperator::LT: hit = v <  c; break;
      case ComparisonOperator::LE: hit = v <= c; break;
      case ComparisonOperator::GT: hit = v >  c; break;
      case ComparisonOperator::GE: hit = v >= c; break;
      case ComparisonOperator::EQ: hit = v == c; break;
      case ComparisonOperator::NE: hit = v != c; break;
    }
    if (hit) p += gc.getProb(wires[i]);
  }
  return p;
}

/**
 * @brief Try to decide @p cmp_gate via a closed-form CDF.
 *
 * Recognised shapes:
 * - @c X @c cmp @c c (X a bare @c gate_rv, c a bare @c gate_value)
 * - @c c @c cmp @c X (mirror of the above; flip the comparator)
 * - @c X @c cmp @c Y where both @c X and @c Y are bare normal
 *   @c gate_rv leaves with distinct UUIDs (independence test)
 *
 * Returns the analytical probability in [0, 1] when decided,
 * @c NaN otherwise.
 */
double tryAnalyticDecide(const GenericCircuit &gc, gate_t cmp_gate)
{
  bool ok = false;
  ComparisonOperator op = cmpOpFromOid(gc.getInfos(cmp_gate).first, ok);
  if (!ok) return std::numeric_limits<double>::quiet_NaN();

  const auto &wires = gc.getWires(cmp_gate);
  if (wires.size() != 2) return std::numeric_limits<double>::quiet_NaN();
  gate_t lhs = wires[0], rhs = wires[1];

  /* X cmp c */
  if (auto specX = bareRv(gc, lhs)) {
    double c = bareValue(gc, rhs);
    if (!std::isnan(c)) return cdfDecide(*specX, op, c);
  }

  /* c cmp X */
  if (auto specX = bareRv(gc, rhs)) {
    double c = bareValue(gc, lhs);
    if (!std::isnan(c)) return cdfDecide(*specX, flipCmpOp(op), c);
  }

  /* Categorical mixture cmp constant: exact sum of mass over the
   * mulinputs whose value satisfies the predicate.  EQ / NE are
   * meaningful on a discrete distribution and decided here rather
   * than the continuous-default route RangeCheck takes. */
  if (gc.isCategoricalMixture(lhs)) {
    double c = bareValue(gc, rhs);
    if (!std::isnan(c)) return categoricalDecide(gc, lhs, op, c);
  }
  if (gc.isCategoricalMixture(rhs)) {
    double c = bareValue(gc, lhs);
    if (!std::isnan(c)) return categoricalDecide(gc, rhs, flipCmpOp(op), c);
  }

  /* X cmp Y, both bare RVs of the same family with a closed form
   * (Normal-Normal, Exp-Exp, Uniform-Uniform).  The @c X cmp X same-UUID case
   * is handled upstream by RangeCheck's identity shortcut, so by the time we
   * get here distinct UUIDs implies independence (each RV constructor mints a
   * fresh @c uuid_generate_v4 token). */
  {
    auto specX = bareRv(gc, lhs);
    auto specY = bareRv(gc, rhs);
    if (specX && specY)
      return rvVsRvDecide(*specX, *specY, op);
  }

  return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

unsigned runAnalyticEvaluator(GenericCircuit &gc)
{
  unsigned resolved = 0;
  const auto nb = gc.getNbGates();

  /* Snapshot the cmp-gate ids so in-place rewrites don't affect the
   * iteration: same pattern as @c runRangeCheck. */
  std::vector<gate_t> cmps;
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) == gate_cmp)
      cmps.push_back(g);
  }

  for (gate_t c : cmps) {
    if (gc.getGateType(c) != gate_cmp) continue;
    double p = tryAnalyticDecide(gc, c);
    if (!std::isnan(p)) {
      /* Clamp to [0, 1] defensively: floating-point CDF roundoff
       * could in principle produce values marginally outside the
       * unit interval (1 - F(c) for c far in the right tail). */
      if (p < 0.0) p = 0.0;
      if (p > 1.0) p = 1.0;
      gc.resolveCmpToBernoulli(c, p);
      ++resolved;
    }
  }

  return resolved;
}

}  // namespace provsql
