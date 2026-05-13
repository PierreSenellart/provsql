/**
 * @file RvSample.cpp
 * @brief SQL function `provsql.rv_sample(token, n, prov)`.
 *
 * Returns up to @c n samples from the (possibly conditional) scalar
 * distribution rooted at @c token.  When @c prov resolves to
 * @c gate_one the samples come from the unconditional distribution
 * (one draw per call to @c monteCarloScalarSamples); when @c prov is
 * a non-trivial gate the path switches to MC rejection via
 * @c monteCarloConditionalScalarSamples, with a budget large enough
 * to deliver @c n accepted draws under the @c acceptance_floor
 * heuristic.
 *
 * Result: @c SETOF @c float8 emitted through the Materialize SRF
 * pattern (same shape as @c shapley_all_vars).  The unconditional
 * path always returns exactly @c n rows; the conditional path may
 * return fewer, in which case a @c NOTICE is emitted so the caller
 * can choose to widen the budget by raising
 * @c provsql.rv_mc_samples.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_sample);
}

#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "MonteCarloSampler.h"
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <vector>

extern "C" Datum
rv_sample(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext = MemoryContextSwitchTo(per_query_ctx);

  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore = tuplestore_begin_heap(
    rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  try {
    pg_uuid_t *token = (pg_uuid_t *) PG_GETARG_POINTER(0);
    const int32 n_signed = PG_GETARG_INT32(1);
    pg_uuid_t *prov  = (pg_uuid_t *) PG_GETARG_POINTER(2);

    if (n_signed <= 0)
      provsql_error("rv_sample: n must be positive (got %d)", n_signed);
    const unsigned n = static_cast<unsigned>(n_signed);

    gate_t root_gate, event_gate;
    auto gc = getJointCircuit(*token, *prov, root_gate, event_gate);

    const bool conditional = gc.getGateType(event_gate) != gate_one;

    std::vector<double> samples;
    if (conditional) {
      /* Closed-form truncation fast path: when the root is a bare
       * gate_rv of a supported family (Uniform / Normal / Exponential)
       * and the event reduces to a single interval on it, we draw
       * exactly @c n samples directly from the truncated distribution.
       * 100% acceptance, no NOTICE on tight events like X > 9.5 over
       * U(0, 10) that the MC rejection path degrades on.  Falls
       * through to the MC rejection path for un-extractable shapes
       * (Erlang, gate_arith composites, gate_mixture roots, …). */
      auto direct = provsql::try_truncated_closed_form_sample(
                      gc, root_gate, event_gate, n);
      if (direct) {
        samples = std::move(*direct);
      } else {
        /* Budget: n / acceptance_floor candidate draws, capped at the
         * GUC ceiling.  acceptance_floor = 0.001 means a 0.1% acceptance
         * rate still delivers n samples; rates below that yield fewer
         * samples + a NOTICE. */
        const unsigned budget = std::min(
          static_cast<unsigned>(1000u) * n,
          provsql_rv_mc_samples > 0
            ? static_cast<unsigned>(provsql_rv_mc_samples) : 1000u * n);
        auto cs = provsql::monteCarloConditionalScalarSamples(
                    gc, root_gate, event_gate, budget);
        if (cs.accepted.size() > n) cs.accepted.resize(n);
        if (cs.accepted.size() < n) {
          ereport(NOTICE,
                  (errmsg("rv_sample: requested %u, returning %zu "
                          "(acceptance rate %zu/%u)",
                          n, cs.accepted.size(),
                          cs.accepted.size(), cs.attempted)));
        }
        samples = std::move(cs.accepted);
      }
    } else {
      samples = provsql::monteCarloScalarSamples(gc, root_gate, n);
    }

    for (double x : samples) {
      Datum values[1] = { Float8GetDatum(x) };
      bool nulls[1] = { false };
      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  } catch (const std::exception &e) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("rv_sample: %s", e.what());
  } catch (...) {
    MemoryContextSwitchTo(oldcontext);
    provsql_error("rv_sample: unknown exception");
  }

  MemoryContextSwitchTo(oldcontext);
  PG_RETURN_NULL();
}
