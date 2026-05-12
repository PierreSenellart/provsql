/**
 * @file RvHistogram.cpp
 * @brief SQL function `provsql.rv_histogram(token, bins)`.
 *
 * Returns an empirical histogram of the scalar values produced by the
 * sub-circuit rooted at @c token, binned into @p bins equal-width
 * intervals between the observed minimum and maximum.  The sample
 * count is taken from @c provsql.rv_mc_samples; pinning
 * @c provsql.monte_carlo_seed makes the result reproducible.
 *
 * Output: a jsonb array of objects @c {bin_lo, bin_hi, count}, one per
 * non-empty bin (zero-count bins are still emitted so the client can
 * draw a flat baseline).  Returning jsonb rather than @c SETOF record
 * keeps the C++ side free of SRF / FuncCallContext mechanics and
 * matches the pattern used by @c simplified_circuit_subgraph.
 *
 * Accepted root gate types:
 *   - @c gate_value: degenerate Dirac at the constant.  Emits a single
 *     bin at @c (v, v) with count equal to the would-be sample count,
 *     so the client can normalise to a probability mass without a
 *     special case.
 *   - @c gate_rv: sampled from the leaf's distribution.
 *   - @c gate_arith: sampled by recursing through the arithmetic DAG,
 *     reusing @c gate_rv draws within an iteration so shared leaves
 *     are correctly correlated.
 *   - @c gate_mixture: sampled by recursing through the mixture's
 *     Bernoulli (gate_input) wire and the selected scalar branch,
 *     reusing per-iteration caches so a shared p_token across the
 *     circuit produces coupled draws.
 *
 * Any other gate type raises: probability of a Boolean-valued gate is
 * a scalar that the existing @c probability_evaluate dispatch covers,
 * and aggregation roots have their own moment family in
 * @c agg_raw_moment.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/jsonb.h"
#include "utils/fmgrprotos.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_histogram);
}

#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "MonteCarloSampler.h"
#include "RandomVariable.h"
#include "RangeCheck.h"
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void emit_bin(std::ostringstream &out, bool &first,
              double lo, double hi, unsigned count)
{
  if (!first) out << ',';
  first = false;
  out << "{\"bin_lo\":" << lo
      << ",\"bin_hi\":" << hi
      << ",\"count\":" << count << '}';
}

}  // namespace

extern "C" Datum
rv_histogram(PG_FUNCTION_ARGS)
{
  pg_uuid_t *root_arg = (pg_uuid_t *) PG_GETARG_POINTER(0);
  int bins = PG_GETARG_INT32(1);
  pg_uuid_t *prov_arg = (pg_uuid_t *) PG_GETARG_POINTER(2);

  if (bins <= 0)
    provsql_error("rv_histogram: bins must be positive (got %d)", bins);

  std::ostringstream out;
  /* setprecision(17) preserves full double round-trip through the
   * jsonb_in parser, so the client sees the same bin edges that the
   * sampler produced. */
  out << std::setprecision(17);
  out << '[';
  bool first = true;

  try {
    /* Always go through getJointCircuit: when prov is gate_one() the
     * joint loader still produces a valid single-root closure (the
     * gate_one leaf is just an extra disconnected node).  This keeps
     * a single code path for shared-leaf coupling between the
     * indicator (event_gate) and the value (root_gate) in the
     * conditional case. */
    gate_t root_gate, event_gate;
    GenericCircuit gc;
    try {
      gc = getJointCircuit(*root_arg, *prov_arg, root_gate, event_gate);
    } catch (const CircuitException &) {
      out << ']';
      Datum json = DirectFunctionCall1(
        jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
      PG_RETURN_DATUM(json);
    }

    /* gate_one event = unconditional. */
    const bool conditional = gc.getGateType(event_gate) != gate_one;
    std::optional<gate_t> event_opt;
    if (conditional) event_opt = event_gate;

    const gate_type t = gc.getGateType(root_gate);

    if (t == gate_value) {
      /* Dirac: parse the literal, emit one degenerate bin.  Count
       * mirrors rv_mc_samples (or 1 if MC is disabled) so the client
       * can compute relative mass the same way for every gate kind. */
      const double v = provsql::parseDoubleStrict(gc.getExtra(root_gate));
      const unsigned n = provsql_rv_mc_samples > 0
                         ? static_cast<unsigned>(provsql_rv_mc_samples)
                         : 1u;
      emit_bin(out, first, v, v, n);
    } else if (t == gate_rv || t == gate_arith || t == gate_mixture) {
      if (provsql_rv_mc_samples <= 0)
        provsql_error(
          "rv_histogram: provsql.rv_mc_samples = 0 disables sampling; "
          "raise it above 0 to compute a histogram");
      const unsigned N = static_cast<unsigned>(provsql_rv_mc_samples);

      std::vector<double> samples;
      if (conditional) {
        auto cs = provsql::monteCarloConditionalScalarSamples(
                    gc, root_gate, event_gate, N);
        if (cs.accepted.empty())
          provsql_error(
            "rv_histogram: conditional MC accepted 0 of %u samples; "
            "raise provsql.rv_mc_samples or check that the event is satisfiable",
            cs.attempted);
        samples = std::move(cs.accepted);
      } else {
        samples = provsql::monteCarloScalarSamples(gc, root_gate, N);
      }

      if (!samples.empty()) {
        /* Pick the bin range per side: when @c compute_support proves
         * a finite support endpoint we use it verbatim (Uniform / sums
         * of Uniforms / mixtures of bounded RVs / Exponential's lower
         * end at 0 / etc.), because the analytical bound is tighter
         * than any sample-based estimate.  When the support is open
         * on a side (Normal, sums involving Normal, the upper tail of
         * Exponential / Erlang) the raw empirical extreme would be
         * an outlier draw -- ~±4σ for a Normal at rv_mc_samples = 10000
         * -- which stretches the histogram so the bulk of the mass
         * concentrates in middle bins and the edge bins look empty.
         * Trim the outermost 0.1% of samples on that side; samples
         * outside the trimmed range still get pooled into the edge
         * bin (the clamp below), so total counts stay conserved. */
        std::sort(samples.begin(), samples.end());
        const std::size_t n = samples.size();
        const std::size_t lo_idx = n / 1000;             /* 0.1% */
        const std::size_t hi_idx = n - 1 - lo_idx;
        auto support = provsql::compute_support(gc, root_gate, event_opt);
        double smin = std::isfinite(support.first)
                    ? support.first
                    : samples[lo_idx];
        double smax = std::isfinite(support.second)
                    ? support.second
                    : samples[hi_idx];
        if (smin == smax) {
          /* Degenerate: trimmed range collapsed to a point (every
           * non-tail draw is the same value, or the simplifier
           * elided everything but a single point-mass leaf). */
          emit_bin(out, first, smin, smax,
                   static_cast<unsigned>(samples.size()));
        } else {
          std::vector<unsigned> counts(bins, 0);
          const double width = (smax - smin) / bins;
          for (double x : samples) {
            int b = static_cast<int>((x - smin) / width);
            if (b < 0) b = 0;
            if (b >= bins) b = bins - 1;
            counts[b] += 1;
          }
          for (int i = 0; i < bins; ++i) {
            const double lo = smin + i * width;
            const double hi = (i == bins - 1) ? smax : lo + width;
            emit_bin(out, first, lo, hi, counts[i]);
          }
        }
      }
    } else {
      const char *type_name = (t < nb_gate_types)
                              ? gate_type_name[t] : "invalid";
      provsql_error(
        "rv_histogram: root gate type '%s' is not a scalar "
        "(expected gate_value, gate_rv, gate_arith, or gate_mixture)",
        type_name);
    }
  } catch (const std::exception &e) {
    provsql_error("rv_histogram: %s", e.what());
  } catch (...) {
    provsql_error("rv_histogram: unknown exception");
  }

  out << ']';
  Datum json = DirectFunctionCall1(
    jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
  PG_RETURN_DATUM(json);
}
