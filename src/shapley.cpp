/**
 * @file shapley.cpp
 * @brief SQL functions for Shapley and Banzhaf power-index computation.
 *
 * Implements two SQL-callable functions:
 * - @c provsql.shapley(token, variable, method, args): Shapley value of
 *   a given input gate (tuple) in the provenance circuit rooted at @p token.
 * - @c provsql.shapley_all_vars(token, method, args): Shapley values for
 *   all input gates simultaneously (more efficient than calling @c shapley()
 *   once per variable).
 *
 * The @p method argument selects the d-DNNF construction:
 * - empty / @c "default" / @c "auto": cost-select the cheapest route via the
 *   probability catalog's chooser.
 * - @c "tree-decomposition": exact, polynomial if treewidth ≤ @c MAX_TREEWIDTH.
 * - @c "interpret-as-dd": direct interpretation of the circuit as a d-DNNF.
 * - @c "compilation": external knowledge compiler, named in @p args
 *   (@c "d4", @c "c2d", …); empty @p args picks the highest-preference one.
 *
 * Banzhaf power index computation is exposed via the same internal helper
 * (@c shapley_internal with @c banzhaf=true), called by the
 * @c provsql.banzhaf() SQL function defined in the SQL layer.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(shapley);
PG_FUNCTION_INFO_V1(shapley_all_vars);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "ProbabilityMethod.h"
#include "GenericCircuit.h"
#include "Circuit.hpp"
#include <unordered_map>
#include "provsql_utils_cpp.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "CircuitFromMMap.h"
#include "tool_registry_sync.h"
#include <fstream>

using namespace std;

/**
 * @brief Core implementation for Shapley and Banzhaf index computation.
 * @param token     UUID of the root provenance gate.
 * @param variable  UUID of the input gate whose index is to be computed.
 * @param method    d-DNNF compilation method.
 * @param args      Additional arguments for the compilation method.
 * @param banzhaf   If @c true, compute the Banzhaf index instead of Shapley.
 * @return          The Shapley (or Banzhaf) value of @p variable.
 */
static double shapley_internal
  (pg_uuid_t token, pg_uuid_t variable, const std::string &method, const std::string &args, bool banzhaf)
{
  /* A conditioned token (X | C) is refused: Shapley / Banzhaf are linear in
   * their value function, whereas P(X|C) = P(X∧C)/P(C) is a non-linear ratio,
   * so the conditional indices are not a combination of the unconditioned
   * ones and have no implementation here.  Detect the conditioned root and
   * raise a Shapley-specific message rather than the generic semiring-refusal
   * thrown deeper in the Boolean-circuit build. */
  GenericCircuit gc = getGenericCircuit(token);
  if(gc.getGateType(gc.getGate(uuid2string(token))) == gate_conditioned)
    provsql_error("shapley/banzhaf: conditional Shapley / Banzhaf values are "
                  "not supported -- a conditioned token (X | C) cannot be "
                  "passed to shapley() / banzhaf().  Compute the index on the "
                  "unconditioned token, or use probability_evaluate for the "
                  "conditional probability P(X|C)");
  gate_t root;
  std::unordered_map<gate_t, gate_t> gc_to_bc;
  BooleanCircuit c = getBooleanCircuit(gc, token, root, gc_to_bc);

  if(c.hasMultivaluedGates())
    provsql_error("Computing Shapley/Banzhaf values is ill-defined for circuits with multivalued (mulinput) gates");

  if(c.getGateType(c.getGate(uuid2string(variable))) != BooleanGate::IN)
    return 0.;

  // Default / "auto": cost-select the d-D construction (interpret-as-dd /
  // tree-decomposition / compilation) via the probability catalog's chooser;
  // any other method (tree-decomposition / interpret-as-dd / compilation, the
  // latter with a compiler name in `args`) is taken by name.
  dDNNF dd = (method.empty() || method == "default" || method == "auto")
               ? provsql::makeDDAuto(c, root)
               : c.makeDD(root, method, args);

  dd.makeSmooth();
  if(!banzhaf)
    dd.makeGatesBinary(BooleanGate::AND);

  auto var_gate=dd.getGate(uuid2string(variable));

  double result;

  if(!banzhaf)
    result = dd.shapley(var_gate);
  else
    result = dd.banzhaf(var_gate);

  return result;
}

/** @brief PostgreSQL-callable wrapper for shapley() and banzhaf(). */
Datum shapley(PG_FUNCTION_ARGS)
{
  provsql_sync_tool_registry();  // honour persisted tool-registry overrides
  try {
    if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
      PG_RETURN_NULL();

    Datum token = PG_GETARG_DATUM(0);
    Datum variable = PG_GETARG_DATUM(1);

    std::string method;
    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    std::string args;
    if(!PG_ARGISNULL(3)) {
      text *t = PG_GETARG_TEXT_P(3);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    bool banzhaf = false;
    if(!PG_ARGISNULL(4)) {
      banzhaf = PG_GETARG_BOOL(4);
    }

    PG_RETURN_FLOAT8(shapley_internal(*DatumGetUUIDP(token), *DatumGetUUIDP(variable), method, args, banzhaf));
  } catch(const std::exception &e) {
    provsql_error("shapley: %s", e.what());
  } catch(...) {
    provsql_error("shapley: Unknown exception");
  }

  PG_RETURN_NULL();
}

/** @brief PostgreSQL-callable wrapper for shapley_all_vars() set-returning function. */
Datum shapley_all_vars(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext    = MemoryContextSwitchTo(per_query_ctx);

  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore     = tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  if(!PG_ARGISNULL(0)) {
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));

    std::string method;
    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    std::string args;
    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    bool banzhaf = false;
    if(!PG_ARGISNULL(3)) {
      banzhaf = PG_GETARG_BOOL(3);
    }


    GenericCircuit gc = getGenericCircuit(token);
    if(gc.getGateType(gc.getGate(uuid2string(token))) == gate_conditioned)
      provsql_error("shapley/banzhaf: conditional Shapley / Banzhaf values are "
                    "not supported -- a conditioned token (X | C) cannot be "
                    "passed to shapley() / banzhaf().  Compute the index on the "
                    "unconditioned token, or use probability_evaluate for the "
                    "conditional probability P(X|C)");
    gate_t root;
    std::unordered_map<gate_t, gate_t> gc_to_bc;
    BooleanCircuit c = getBooleanCircuit(gc, token, root, gc_to_bc);

    if(c.hasMultivaluedGates())
      provsql_error("Computing Shapley/Banzhaf values is ill-defined for circuits with multivalued (mulinput) gates");

    dDNNF dd = (method.empty() || method == "default" || method == "auto")
                 ? provsql::makeDDAuto(c, root)
                 : c.makeDD(root, method, args);
    dd.makeSmooth();
    if(!banzhaf)
      dd.makeGatesBinary(BooleanGate::AND);

    for(auto &v_circuit_gate: c.getInputs()) {
      auto var_uuid_string = c.getUUID(v_circuit_gate);
      auto var_gate=dd.getGate(var_uuid_string);
      pg_uuid_t *uuidp = reinterpret_cast<pg_uuid_t*>(palloc(UUID_LEN));
      *uuidp = string2uuid(var_uuid_string);

      double result;

      if(!banzhaf)
        result = dd.shapley(var_gate);
      else
        result = dd.banzhaf(var_gate);

      Datum values[2] = {
        UUIDPGetDatum(uuidp), Float8GetDatum(result)
      };
      bool nulls[sizeof(values)] = {0, 0};

      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  }

  MemoryContextSwitchTo(oldcontext);

  PG_RETURN_NULL();
}
