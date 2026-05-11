/**
 * @file AnalyticEvaluator.cpp
 * @brief Implementation of the closed-form CDF resolution pass.
 *        See @c AnalyticEvaluator.h for the full docstring.
 */
#include "AnalyticEvaluator.h"

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "Aggregation.h"        // ComparisonOperator + cmpOpFromOid
#include "RandomVariable.h"     // parse_distribution_spec, parseDoubleStrict, DistKind
extern "C" {
#include "provsql_utils.h"      // gate_type
}

namespace provsql {

namespace {

/* All four ordered comparators reduce to either F(c) or 1 - F(c)
 * (continuous: @c < and @c <= have the same probability, ditto @c >
 * and @c >=).  EQ / NE on continuous RVs are handled universally by
 * RangeCheck (P(X = c) = 0, P(X != c) = 1, sound in every semiring
 * via gate_zero / gate_one); they should never reach this function.
 *
 * CDFs come from @c <cmath>: @c std::erf for the normal,
 * @c std::expm1 for the exponential, plain arithmetic for the
 * uniform.  Standard-library precision (~1 ULP on modern platforms)
 * is well within the 1e-12 tolerance the regression tests pin. */
double cdfDecide(const DistributionSpec &d, ComparisonOperator op, double c)
{
  double cdf_c = std::numeric_limits<double>::quiet_NaN();
  switch (d.kind) {
    case DistKind::Normal: {
      /* Φ((c - μ)/σ) = ½ (1 + erf((c - μ) / (σ √2))).  Standard
       * formula; std::erf is C99 / C++11. */
      static const double SQRT2 = std::sqrt(2.0);
      double z = (c - d.p1) / d.p2;
      cdf_c = 0.5 * (1.0 + std::erf(z / SQRT2));
      break;
    }
    case DistKind::Uniform:
      if (c <= d.p1)        cdf_c = 0.0;
      else if (c >= d.p2)   cdf_c = 1.0;
      else                  cdf_c = (c - d.p1) / (d.p2 - d.p1);
      break;
    case DistKind::Exponential:
      if (c <= 0.0) cdf_c = 0.0;
      else          cdf_c = -std::expm1(-d.p1 * c);  /* 1 - exp(-λc) */
      break;
    case DistKind::Erlang: {
      /* For integer shape s ≥ 1, the Erlang CDF has the finite-sum
       * form F(c; s, λ) = 1 - e^{-λc} Σ_{n=0..s-1} (λc)^n / n!.
       * Numerically stable for the small-to-moderate s the simplifier
       * produces (a SUM of k i.i.d. Exp gates).  Non-integer s would
       * require the regularised lower incomplete gamma function, so
       * we skip those cases by leaving cdf_c = NaN (caller treats NaN
       * as "undecided" and the cmp falls through to MC). */
      const double s = d.p1, lambda = d.p2;
      if (s < 1.0 || s != std::floor(s)) break;
      if (c <= 0.0) { cdf_c = 0.0; break; }
      const double lc = lambda * c;
      double term = 1.0;   /* (λc)^0 / 0! */
      double sum  = 1.0;
      const unsigned long k = static_cast<unsigned long>(s);
      for (unsigned long n = 1; n < k; ++n) {
        term *= lc / static_cast<double>(n);
        sum  += term;
      }
      cdf_c = 1.0 - std::exp(-lc) * sum;
      break;
    }
  }
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

  /* X cmp Y, both bare normal RVs.  The @c X cmp X same-UUID case
   * is handled upstream by RangeCheck's identity shortcut, so by the
   * time we get here distinct UUIDs implies independence (each
   * @c provsql.normal call mints a fresh @c uuid_generate_v4 token). */
  {
    auto specX = bareRv(gc, lhs);
    auto specY = bareRv(gc, rhs);
    if (specX && specY &&
        specX->kind == DistKind::Normal &&
        specY->kind == DistKind::Normal) {
      return normalDiffDecide(*specX, *specY, op);
    }
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
