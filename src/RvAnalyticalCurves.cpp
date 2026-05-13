/**
 * @file RvAnalyticalCurves.cpp
 * @brief SQL function `provsql.rv_analytical_curves(token, samples, prov)`.
 *
 * Returns a JSON object @c {"pdf": [{x, p}, ...], "cdf": [{x, p}, ...]}
 * with @p samples evenly-spaced points spanning the closed-form support
 * of the (possibly truncated) scalar distribution rooted at @p token,
 * or @c NULL when no closed form applies.
 *
 * Used by ProvSQL Studio's Distribution profile panel to overlay the
 * analytical PDF / CDF on the empirical histogram drawn from
 * @c rv_histogram.  The overlay makes the simplifier's analytical
 * win visible: when @c c·Exp(λ) folds to @c Exp(λ/c) the panel shows
 * a smooth exponential decay rather than only noisy MC bars.
 *
 * Supported shapes (V1):
 *   - bare @c gate_rv root of Normal / Uniform / Exponential /
 *     Erlang (integer shape), unconditional.
 *   - same with a one-interval conditioning event extractable by
 *     @c collectRvConstraints: the truncated PDF is
 *     @c pdfAt(d, x) / Z with @c Z = @c CDF(hi) - @c CDF(lo).
 *
 * Out of scope (returns @c NULL): @c gate_arith composites,
 * @c gate_mixture (Bernoulli or categorical), non-integer Erlang
 * shapes.  Bernoulli mixtures over closed-form arms and categoricals
 * are natural follow-ups (weighted sum of children's PDFs / stem
 * plot over outcome values respectively).
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
#include "GenericCircuit.h"
#include "RandomVariable.h"     // parse_distribution_spec, DistKind
#include "RangeCheck.h"         // collectRvConstraints
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace {

/**
 * @brief Choose a sensible x-range for the curve given a distribution
 *        spec and an optional truncation.
 *
 * Unbounded distributions (Normal) get a heuristic window around the
 * mean; bounded distributions (Uniform) get a slight padding so the
 * support boundary doesn't sit flush with the SVG edge; one-sided
 * supports (Exponential, Erlang) get @c 6/λ on the right.
 *
 * Truncation clamps the window: the curve never extends past the
 * conditioning event's interval.  For a heavily-truncated normal
 * (say @c |X| > 5) the window collapses to the conditioning interval
 * directly.
 */
std::pair<double, double>
choose_x_range(const provsql::DistributionSpec &spec,
               double trunc_lo, double trunc_hi)
{
  double lo = trunc_lo, hi = trunc_hi;
  switch (spec.kind) {
    case provsql::DistKind::Normal: {
      const double mu = spec.p1, sigma = spec.p2;
      if (!std::isfinite(lo)) lo = mu - 4.0 * sigma;
      if (!std::isfinite(hi)) hi = mu + 4.0 * sigma;
      break;
    }
    case provsql::DistKind::Uniform: {
      const double a = spec.p1, b = spec.p2;
      const double pad = 0.15 * (b - a);
      if (!std::isfinite(lo)) lo = a - pad;
      else                    lo = std::max(lo, a - pad);
      if (!std::isfinite(hi)) hi = b + pad;
      else                    hi = std::min(hi, b + pad);
      break;
    }
    case provsql::DistKind::Exponential: {
      const double lambda = spec.p1;
      if (!std::isfinite(lo)) lo = 0.0;
      if (!std::isfinite(hi)) hi = 6.0 / lambda;
      break;
    }
    case provsql::DistKind::Erlang: {
      const double k = spec.p1, lambda = spec.p2;
      if (!std::isfinite(lo)) lo = 0.0;
      if (!std::isfinite(hi)) hi = std::max(2.0 * k / lambda, 6.0 / lambda);
      break;
    }
  }
  return {lo, hi};
}

}  // namespace

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

    /* V1: bare gate_rv root only.  gate_arith composites, mixtures,
     * categoricals -- follow-ups.  Falling through to NULL signals the
     * frontend "no overlay" without raising. */
    if (gc.getGateType(root_gate) != gate_rv) PG_RETURN_NULL();

    auto spec = provsql::parse_distribution_spec(gc.getExtra(root_gate));
    if (!spec) PG_RETURN_NULL();

    /* Truncation interval, if any.  collectRvConstraints already
     * intersects with the RV's natural support and normalises an
     * infeasible event to a single point.  We treat any degenerate
     * interval as "skip overlay" so the panel falls back to the
     * histogram-only rendering.  @c gate_zero events (an infeasible
     * cmp resolved by RangeCheck) bypass the walker entirely (it
     * skips gate_zero leaves the same way it skips gate_one), so
     * @c collectRvConstraints would return the unconditional support
     * for those; treat them as NULL explicitly. */
    double trunc_lo = -std::numeric_limits<double>::infinity();
    double trunc_hi = +std::numeric_limits<double>::infinity();
    const gate_type event_t = gc.getGateType(event_gate);
    if (event_t == gate_zero) PG_RETURN_NULL();
    const bool conditional = event_t != gate_one;
    if (conditional) {
      auto iv = provsql::collectRvConstraints(gc, event_gate, root_gate);
      if (!iv.has_value()) PG_RETURN_NULL();
      if (!(iv->first < iv->second)) PG_RETURN_NULL();
      trunc_lo = iv->first;
      trunc_hi = iv->second;
    }

    /* Normalisation Z = CDF(hi) - CDF(lo).  Unconditional case: Z = 1
     * trivially (lo = -inf -> CDF = 0, hi = +inf -> CDF = 1).  Truncated:
     * the closed-form CDFs in AnalyticEvaluator handle finite endpoints
     * exactly; an unbounded end maps to CDF = 0 / 1 by construction. */
    const double cdf_lo = std::isfinite(trunc_lo) ? provsql::cdfAt(*spec, trunc_lo) : 0.0;
    const double cdf_hi = std::isfinite(trunc_hi) ? provsql::cdfAt(*spec, trunc_hi) : 1.0;
    const double Z = cdf_hi - cdf_lo;
    if (!(Z > 0.0)) PG_RETURN_NULL();   /* infeasible event */

    /* Pick the x-range and sample. */
    auto [x_lo, x_hi] = choose_x_range(*spec, trunc_lo, trunc_hi);
    if (!(x_lo < x_hi)) PG_RETURN_NULL();

    std::ostringstream out;
    /* setprecision(17) keeps each sample bit-round-trippable through
     * jsonb_in's parser, matching the convention used by rv_histogram
     * for its bin_lo / bin_hi fields. */
    out << std::setprecision(17);
    out << "{\"pdf\":[";

    /* Pre-compute (x, pdf) and (x, cdf) into one pass. */
    std::ostringstream cdf_out;
    cdf_out << std::setprecision(17);
    cdf_out << "\"cdf\":[";

    for (int i = 0; i < samples; ++i) {
      const double t = static_cast<double>(i) / (samples - 1);
      const double x = x_lo + t * (x_hi - x_lo);
      double pdf_x = provsql::pdfAt(*spec, x);
      double cdf_x = provsql::cdfAt(*spec, x);
      if (std::isnan(pdf_x) || std::isnan(cdf_x)) PG_RETURN_NULL();
      /* Truncate to [trunc_lo, trunc_hi] for conditional case: outside
       * the interval pdf = 0 and cdf is clipped to 0 / 1.  Then
       * normalise by Z. */
      if (conditional) {
        if (x < trunc_lo || x > trunc_hi) {
          pdf_x = 0.0;
          cdf_x = (x < trunc_lo) ? 0.0 : 1.0;
        } else {
          pdf_x /= Z;
          cdf_x = (cdf_x - cdf_lo) / Z;
        }
      }
      if (i > 0) { out << ','; cdf_out << ','; }
      out     << "{\"x\":" << x << ",\"p\":" << pdf_x << '}';
      cdf_out << "{\"x\":" << x << ",\"p\":" << cdf_x << '}';
    }
    out << "],";
    cdf_out << ']';
    out << cdf_out.str() << '}';

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
