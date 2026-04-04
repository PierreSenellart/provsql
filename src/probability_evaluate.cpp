/**
 * @file probability_evaluate.cpp
 * @brief SQL function @c provsql.probability_evaluate() – probabilistic circuit evaluation.
 *
 * Implements @c provsql.probability_evaluate(), which computes the
 * probability that a provenance circuit evaluates to @c true under the
 * tuple-independent probabilistic-database model.
 *
 * The @p method argument selects the computation algorithm:
 * - @c "possible-worlds": exact enumeration of all 2^n worlds.
 * - @c "monte-carlo": approximate via random sampling (fast, inexact).
 * - @c "weightmc": approximate using the @c weightmc model counter.
 * - @c "tree-decomposition": exact via tree-decomposition-based d-DNNF.
 * - @c "independent": exact evaluation for disconnected circuits.
 * - Any external compiler name (@c "d4", @c "c2d", @c "minic2d", @c "dsharp").
 *
 * A SIGINT signal sets a process-local flag that causes the evaluation
 * to abort and return @c NULL (used when the user cancels a long-running
 * probability computation).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(probability_evaluate);
}

#include "c_cpp_compatibility.h"
#include <set>
#include <cmath>
#include <csignal>

#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "having_semantics.hpp"
#include "provsql_mmap.h"
#include "provsql_utils_cpp.h"
#include "semiring/BoolExpr.h"

using namespace std;

/**
 * @brief SIGINT handler that sets the global interrupted flag.
 *
 * The signal number argument is required by the @c signal() API but is
 * not used.
 */
static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;
}

/**
 * @brief Core implementation of probability evaluation for a circuit token.
 * @param token   UUID of the root provenance gate.
 * @param method  Evaluation method name (e.g. "independent", "monte-carlo").
 * @param args    Additional arguments for the chosen method.
 * @return        Float8 Datum containing the computed probability.
 */
static Datum probability_evaluate_internal
  (pg_uuid_t token, const string &method, const string &args)
{
  gate_t gate;
  BooleanCircuit c = getBooleanCircuit(token, gate);

  double result;

  // Display the circuit for debugging:
  // elog(WARNING, "%s", c.toString(gate).c_str());

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  try {
    bool processed=false;

    if(method=="independent") {
      result = c.independentEvaluation(gate);
      processed = true;
    } else if(method=="") {
      // Default evaluation, use independent, tree-decomposition, and
      // compilation in order until one works
      try {
        result = c.independentEvaluation(gate);
        processed = true;
      } catch(CircuitException &) {}
    }

    if(!processed) {
      // Other methods do not deal with multivalued input gates, they
      // need to be rewritten
      c.rewriteMultivaluedGates();

      if(method=="monte-carlo") {
        int samples=0;

        try {
          samples = stoi(args);
        } catch(const std::invalid_argument &e) {
        }

        if(samples<=0)
          provsql_error("Invalid number of samples: '%s'", args.c_str());

        result = c.monteCarlo(gate, samples);
      } else if(method=="possible-worlds") {
        if(!args.empty())
          provsql_warning("Argument '%s' ignored for method possible-worlds", args.c_str());

        result = c.possibleWorlds(gate);
      } else if(method=="weightmc") {
        result = c.WeightMC(gate, args);
      } else if(method=="compilation" || method=="tree-decomposition" || method=="") {
        auto dd = c.makeDD(gate, method, args);
        result = dd.probabilityEvaluation();
      } else {
        provsql_error("Wrong method '%s' for probability evaluation", method.c_str());
      }
    }
  } catch(CircuitException &e) {
    provsql_error("%s", e.what());
  }

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);

  // Avoid rounding errors that make probability outside of [0,1]
  if(result>1.)
    result=1.;
  else if(result<0.)
    result=0.;

  PG_RETURN_FLOAT8(result);
}

/** @brief PostgreSQL-callable wrapper for probability_evaluate(). */
Datum probability_evaluate(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    string method;
    string args;

    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();

    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    return probability_evaluate_internal(*DatumGetUUIDP(token), method, args);
  } catch(const std::exception &e) {
    provsql_error("probability_evaluate: %s", e.what());
  } catch(...) {
    provsql_error("probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}
