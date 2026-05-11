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
#include "provsql_utils_cpp.h"

#include <iomanip>
#include <sstream>
#include <string>
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
    GenericCircuit gc = getGenericCircuit(*root_arg);

    gate_t root_gate;
    try {
      root_gate = gc.getGate(uuid2string(*root_arg));
    } catch (const CircuitException &) {
      /* Root not present in the simplified circuit (e.g. a gate that
       * the simplifier rewrote into a different UUID).  Return an
       * empty array so the caller can degrade gracefully. */
      out << ']';
      Datum json = DirectFunctionCall1(
        jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
      PG_RETURN_DATUM(json);
    }

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

      auto samples = provsql::monteCarloScalarSamples(gc, root_gate, N);

      if (!samples.empty()) {
        double smin = samples[0], smax = samples[0];
        for (double x : samples) {
          if (x < smin) smin = x;
          if (x > smax) smax = x;
        }
        if (smin == smax) {
          /* Degenerate: every draw landed on the same value.  Could
           * happen when the simplifier elided everything but a leaf
           * the sampler is forced to evaluate point-wise. */
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
