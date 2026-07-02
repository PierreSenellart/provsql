/**
 * @file RvAnalyticalCurves.cpp
 * @brief SQL function `provsql.rv_analytical_curves(token, samples, prov)`.
 *
 * Returns a JSON object with closed-form curves for the (possibly
 * conditional) distribution rooted at @p token, or @c NULL when no
 * closed form applies.  The payload has up to three fields:
 *
 * - @c pdf – @p samples evenly-spaced @c {x, p} points covering the
 *   continuous part of the distribution.  Absent for pure-discrete
 *   shapes (Dirac / categorical).
 * - @c cdf – same x grid as @c pdf, with cumulative probability.
 * - @c stems – point masses @c {x, p} produced by Dirac (@c gate_value
 *   wrapped as an @c as_random) or categorical roots, or by Dirac/
 *   categorical arms inside a Bernoulli mixture.  Weights propagate
 *   through nested mixtures (each ancestor's @c p / 1-p applies).
 *
 * Used by ProvSQL Studio's Distribution profile panel to overlay
 * analytical curves and point-mass discs on the empirical histogram
 * drawn from @c rv_histogram.
 *
 * Supported shapes:
 *   - bare @c gate_rv root (Normal / Uniform / Exponential / Erlang
 *     with integer shape), optionally truncated by an AND-conjunct
 *     event extracted via @c collectRvConstraints;
 *   - Dirac point (@c gate_value with finite extra, surfaced by
 *     @c provsql.as_random);
 *   - categorical-form @c gate_mixture (one @c {key, mul_1..n});
 *   - classic Bernoulli @c gate_mixture (@c [p_token, x, y]) over any
 *     two recursively-matched shapes; @c p_token must be a bare
 *     @c gate_input (compound Boolean @c p bails).
 *
 * Truncation is honoured only on the bare-RV path; mixtures /
 * categoricals / Diracs are matched only with a trivial event.
 *
 * @see provsql::matchClosedFormDistribution in RangeCheck.h
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/jsonb.h"
#include "utils/fmgrprotos.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_analytical_curves);
}

#include "AnalyticEvaluator.h"  // pdfAt, cdfAt
#include "CircuitFromMMap.h"    // getJointCircuit
#include "Expectation.h"        // lift_conditioning
#include "GenericCircuit.h"
#include "HybridEvaluator.h"    // runHybridSimplifier
#include "RandomVariable.h"     // DistKind
#include "distributions/Distribution.h"       // makeDistribution -> plotRange
#include "RangeCheck.h"         // matchClosedFormDistribution + variant
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

/**
 * @brief Choose a sensible x-range for the continuous curve given a
 *        single-RV spec and an optional truncation.
 *
 * Unbounded distributions (Normal) get a heuristic window around the
 * mean; bounded distributions (Uniform) get a slight padding so the
 * support boundary doesn't sit flush with the SVG edge; one-sided
 * supports (Exponential, Erlang) get @c 6/λ on the right.
 *
 * Truncation clamps the window: the curve never extends past the
 * conditioning event's interval.
 */
std::pair<double, double>
bare_x_range(const provsql::DistributionSpec &spec,
             double trunc_lo, double trunc_hi)
{
  return provsql::makeDistribution(spec)->plotRange(trunc_lo, trunc_hi);
}

/**
 * @brief Per-sample truncated PDF for a single-RV arm.  Returns the
 *        unconditional value when @c truncated == @c false.  Yields
 *        @c NaN when the closed-form PDF doesn't cover the spec
 *        (e.g. non-integer Erlang shape, propagated from
 *        @c provsql::pdfAt).
 */
double bare_pdf(const provsql::TruncatedSingleRv &t, double x)
{
  double p = provsql::pdfAt(t.spec, x);
  if (std::isnan(p)) return std::numeric_limits<double>::quiet_NaN();
  if (!t.truncated) return p;
  if (x < t.lo || x > t.hi) return 0.0;
  const double cdf_lo = std::isfinite(t.lo) ? provsql::cdfAt(t.spec, t.lo) : 0.0;
  const double cdf_hi = std::isfinite(t.hi) ? provsql::cdfAt(t.spec, t.hi) : 1.0;
  const double Z = cdf_hi - cdf_lo;
  if (!(Z > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  return p / Z;
}

double bare_cdf(const provsql::TruncatedSingleRv &t, double x)
{
  double c = provsql::cdfAt(t.spec, x);
  if (std::isnan(c)) return std::numeric_limits<double>::quiet_NaN();
  if (!t.truncated) return c;
  if (x < t.lo) return 0.0;
  if (x > t.hi) return 1.0;
  const double cdf_lo = std::isfinite(t.lo) ? provsql::cdfAt(t.spec, t.lo) : 0.0;
  const double cdf_hi = std::isfinite(t.hi) ? provsql::cdfAt(t.spec, t.hi) : 1.0;
  const double Z = cdf_hi - cdf_lo;
  if (!(Z > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  return (c - cdf_lo) / Z;
}

/**
 * @brief Recursive @c pdf(x) over the @c ClosedFormShape variant.
 *        Dirac / categorical arms contribute 0 (point masses live in
 *        the @c stems channel, not in the continuous PDF).  Mixtures
 *        combine arms linearly with the Bernoulli weight.
 */
double shape_pdf(const provsql::ClosedFormShape &s, double x);
double shape_cdf(const provsql::ClosedFormShape &s, double x);

double shape_pdf(const provsql::ClosedFormShape &s, double x)
{
  return std::visit([&](const auto &v) -> double {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, provsql::TruncatedSingleRv>) {
      return bare_pdf(v, x);
    } else if constexpr (std::is_same_v<T, provsql::DiracShape>) {
      (void)x;
      return 0.0;
    } else if constexpr (std::is_same_v<T, provsql::CategoricalShape>) {
      (void)x;
      return 0.0;
    } else if constexpr (std::is_same_v<T, provsql::BernoulliMixtureShape>) {
      const double pl = shape_pdf(*v.left, x);
      const double pr = shape_pdf(*v.right, x);
      if (std::isnan(pl) || std::isnan(pr))
        return std::numeric_limits<double>::quiet_NaN();
      return v.p * pl + (1.0 - v.p) * pr;
    }
    return std::numeric_limits<double>::quiet_NaN();
  }, s);
}

double shape_cdf(const provsql::ClosedFormShape &s, double x)
{
  return std::visit([&](const auto &v) -> double {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, provsql::TruncatedSingleRv>) {
      return bare_cdf(v, x);
    } else if constexpr (std::is_same_v<T, provsql::DiracShape>) {
      return (x >= v.value) ? 1.0 : 0.0;
    } else if constexpr (std::is_same_v<T, provsql::CategoricalShape>) {
      double sum = 0.0;
      for (const auto &pr : v.outcomes) if (pr.first <= x) sum += pr.second;
      return sum;
    } else if constexpr (std::is_same_v<T, provsql::BernoulliMixtureShape>) {
      const double cl = shape_cdf(*v.left, x);
      const double cr = shape_cdf(*v.right, x);
      if (std::isnan(cl) || std::isnan(cr))
        return std::numeric_limits<double>::quiet_NaN();
      return v.p * cl + (1.0 - v.p) * cr;
    }
    return std::numeric_limits<double>::quiet_NaN();
  }, s);
}

bool shape_has_continuous(const provsql::ClosedFormShape &s)
{
  return std::visit([](const auto &v) -> bool {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, provsql::TruncatedSingleRv>)        return true;
    else if constexpr (std::is_same_v<T, provsql::DiracShape>)          return false;
    else if constexpr (std::is_same_v<T, provsql::CategoricalShape>)    return false;
    else if constexpr (std::is_same_v<T, provsql::BernoulliMixtureShape>)
      return shape_has_continuous(*v.left) || shape_has_continuous(*v.right);
    return false;
  }, s);
}

/**
 * @brief Walk the shape collecting weighted stem points.  @p weight
 *        is the running Bernoulli product from the path root; the
 *        leaf-level mass is multiplied by it so e.g. a Dirac inside
 *        @c mixture(0.3, X, c) appears at @c (c, 0.7).
 */
void shape_stems(const provsql::ClosedFormShape &s, double weight,
                 std::vector<std::pair<double, double>> &out)
{
  std::visit([&](const auto &v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, provsql::TruncatedSingleRv>) {
      (void)v;  // continuous arm: contributes no stems
    } else if constexpr (std::is_same_v<T, provsql::DiracShape>) {
      out.emplace_back(v.value, weight);
    } else if constexpr (std::is_same_v<T, provsql::CategoricalShape>) {
      for (const auto &pr : v.outcomes)
        out.emplace_back(pr.first, weight * pr.second);
    } else if constexpr (std::is_same_v<T, provsql::BernoulliMixtureShape>) {
      shape_stems(*v.left,  weight * v.p,         out);
      shape_stems(*v.right, weight * (1.0 - v.p), out);
    }
  }, s);
}

std::pair<double, double> shape_x_range(const provsql::ClosedFormShape &s)
{
  return std::visit([](const auto &v) -> std::pair<double, double> {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, provsql::TruncatedSingleRv>) {
      return bare_x_range(v.spec, v.lo, v.hi);
    } else if constexpr (std::is_same_v<T, provsql::DiracShape>) {
      /* Pure Dirac: pad ±1 around the point so the disc isn't flush
       * against the SVG edge.  When this Dirac is nested under a
       * mixture, the sibling's range usually dominates. */
      return {v.value - 1.0, v.value + 1.0};
    } else if constexpr (std::is_same_v<T, provsql::CategoricalShape>) {
      double mn =  std::numeric_limits<double>::infinity();
      double mx = -std::numeric_limits<double>::infinity();
      for (const auto &pr : v.outcomes) {
        mn = std::min(mn, pr.first);
        mx = std::max(mx, pr.first);
      }
      const double range = mx - mn;
      const double pad   = range > 0.0 ? 0.1 * range : 1.0;
      return {mn - pad, mx + pad};
    } else if constexpr (std::is_same_v<T, provsql::BernoulliMixtureShape>) {
      const auto L = shape_x_range(*v.left);
      const auto R = shape_x_range(*v.right);
      return {std::min(L.first, R.first), std::max(L.second, R.second)};
    }
    return {0.0, 1.0};
  }, s);
}

}  // namespace

namespace provsql {

// Exact histogram of a closed-form shape: `bins` equal-width bins over the
// shape's natural plotting range, each carrying the analytical probability
// mass cdf(hi) - cdf(lo).  Lets rv_histogram answer for a closed-form
// distribution (e.g. a truncated Gaussian) without sampling -- in
// particular under provsql.rv_mc_samples = 0.  Returns nullopt when the
// range is degenerate or the CDF is unavailable (non-integer Erlang, an
// unmatched arm), so the caller can fall back to Monte Carlo.
std::optional<std::vector<std::tuple<double, double, double>>>
analyticalHistogram(const ClosedFormShape &shape, int bins)
{
  if (bins <= 0) return std::nullopt;
  const auto [xlo, xhi] = shape_x_range(shape);
  if (!(xlo < xhi) || !std::isfinite(xlo) || !std::isfinite(xhi))
    return std::nullopt;
  const double w = (xhi - xlo) / bins;
  std::vector<std::tuple<double, double, double>> out;
  out.reserve(bins);
  for (int i = 0; i < bins; ++i) {
    const double lo = xlo + i * w;
    const double hi = (i == bins - 1) ? xhi : lo + w;
    const double cl = shape_cdf(shape, lo);
    const double ch = shape_cdf(shape, hi);
    if (std::isnan(cl) || std::isnan(ch)) return std::nullopt;
    out.emplace_back(lo, hi, std::max(0.0, ch - cl));
  }
  return out;
}

}  // namespace provsql

extern "C" Datum
rv_analytical_curves(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = (pg_uuid_t *) PG_GETARG_POINTER(0);
  int32 samples = PG_GETARG_INT32(1);
  pg_uuid_t *prov  = (pg_uuid_t *) PG_GETARG_POINTER(2);

  if (samples < 2)
    provsql_error(
      "rv_analytical_curves: samples must be at least 2 (got %d)",
      samples);

  try {
    gate_t root_gate, event_gate;
    GenericCircuit gc;
    try {
      gc = getJointCircuit(*token, *prov, root_gate, event_gate);
    } catch (const CircuitException &) {
      PG_RETURN_NULL();
    }

    /* A stored "X | C" arrives as a conditioned root: peel it to the bare
     * scalar target and fold the condition into the event, so the closed-form
     * match below sees the bare distribution truncated by the event rather
     * than a gate_conditioned it cannot match (which would drop to
     * histogram-only). */
    std::optional<gate_t> event_opt;
    if (gc.getGateType(event_gate) != gate_one) event_opt = event_gate;
    root_gate = provsql::lift_conditioning(gc, root_gate, event_opt);

    /* Run the hybrid-evaluator simplifier so the analytical curves
     * see the same folded tree Studio's circuit view shows via
     * simplified_circuit_subgraph: c·Exp(λ) → Exp(λ/c), N(μ,σ)+N(...)
     * → single normal, Erlang sums, etc.  Without this pass the
     * c·Exp(λ) root would be a gate_arith composite that
     * matchClosedFormDistribution does not match, so the panel would
     * silently fall back to histogram-only on a circuit that looks
     * like a single Exp node. */
    if (provsql_hybrid_evaluation)
      provsql::runHybridSimplifier(gc);

    /* Generalised closed-form match: bare RV (with optional
     * truncation), Dirac, categorical, or Bernoulli mixture over any
     * recursively-matched shape.  Non-matched shapes (gate_arith
     * composites, mismatched Erlang shapes, ...) fall through to
     * NULL so the front-end renders histogram-only without a
     * structural pre-check. */
    auto shape = provsql::matchClosedFormDistribution(
                   gc, root_gate, event_opt);
    if (!shape) PG_RETURN_NULL();

    std::vector<std::pair<double, double>> stems;
    shape_stems(*shape, 1.0, stems);
    const bool has_cont = shape_has_continuous(*shape);

    /* Nothing to render: shouldn't normally happen (shape matched
     * but produced neither continuous nor discrete output), but
     * guards against an empty stem list from a categorical with all
     * zero-mass outcomes etc. */
    if (!has_cont && stems.empty()) PG_RETURN_NULL();

    /* x-range chosen over the full shape; for a mixture this is the
     * union of branch ranges, so the curve covers both modes.  A
     * pure-stems shape still gets a small window for the chart axis. */
    auto [x_lo, x_hi] = shape_x_range(*shape);
    if (!(x_lo < x_hi)) PG_RETURN_NULL();

    std::ostringstream out;
    /* setprecision(17) keeps each sample bit-round-trippable through
     * jsonb_in's parser, matching the convention used by rv_histogram
     * for its bin_lo / bin_hi fields. */
    out << std::setprecision(17);
    out << '{';
    bool first_field = true;

    /* CDF is well-defined for every supported shape (a staircase for
     * pure-discrete, a smooth curve for continuous, a curve-with-
     * jumps for mixed), so emit it unconditionally.  PDF is only
     * meaningful when there's a continuous component; for pure
     * point-mass shapes the pdf samples would all be zero and the
     * smooth overlay path would be meaningless. */
    std::ostringstream pdf_out;
    pdf_out << std::setprecision(17);
    if (has_cont) pdf_out << "\"pdf\":[";
    out << "\"cdf\":[";
    for (int i = 0; i < samples; ++i) {
      const double t = static_cast<double>(i) / (samples - 1);
      const double x = x_lo + t * (x_hi - x_lo);
      const double cdf_x = shape_cdf(*shape, x);
      if (std::isnan(cdf_x)) PG_RETURN_NULL();
      if (i > 0) out << ',';
      out << "{\"x\":" << x << ",\"p\":" << cdf_x << '}';
      if (has_cont) {
        const double pdf_x = shape_pdf(*shape, x);
        if (std::isnan(pdf_x)) PG_RETURN_NULL();
        if (i > 0) pdf_out << ',';
        pdf_out << "{\"x\":" << x << ",\"p\":" << pdf_x << '}';
      }
    }
    out << ']';
    if (has_cont) {
      pdf_out << ']';
      out << ',' << pdf_out.str();
    }
    first_field = false;

    if (!stems.empty()) {
      if (!first_field) out << ',';
      out << "\"stems\":[";
      for (std::size_t i = 0; i < stems.size(); ++i) {
        if (i > 0) out << ',';
        out << "{\"x\":" << stems[i].first
            << ",\"p\":" << stems[i].second << '}';
      }
      out << ']';
    }
    out << '}';

    Datum json = DirectFunctionCall1(
      jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
    PG_RETURN_DATUM(json);
  } catch (const std::exception &e) {
    provsql_error("rv_analytical_curves: %s", e.what());
  } catch (...) {
    provsql_error("rv_analytical_curves: unknown exception");
  }
  PG_RETURN_NULL();
}
