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
#include "miscadmin.h"
#include "storage/latch.h"
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
#include "AnalyticEvaluator.h"
#include "HybridEvaluator.h"
#include "RangeCheck.h"
#include "MonteCarloSampler.h"
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
 *
 * In addition to the @c provsql_interrupted flag polled by the long
 * Monte-Carlo / possible-worlds evaluation loops, we drive PG's
 * standard cancel pipeline (@c InterruptPending / @c QueryCancelPending
 * + @c SetLatch) the same way PG's own @c StatementCancelHandler does.
 * That makes a SIGINT delivered to the backend (e.g. via
 * @c pg_cancel_backend) outside of a @c system() wait turn into a
 * proper 57014 cancel at the next @c CHECK_FOR_INTERRUPTS instead of
 * being silently absorbed.  (The matching case where @c system() is
 * blocked when the timer's @c kill(-MyProcPid, SIGINT) fires is
 * handled in @c BooleanCircuit::compilation, since glibc @c system()
 * temporarily @c SIG_IGNs SIGINT in the parent so this handler does
 * not run for it.)
 */
static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;

  if (!proc_exit_inprogress) {
    InterruptPending = true;
    QueryCancelPending = true;
  }
  SetLatch(MyLatch);
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
  // Load the GenericCircuit once: we need it for the RV-detection
  // dispatch below, and getBooleanCircuit() reuses it internally so we
  // pay no extra cost compared to the previous flow.  Universal
  // cmp-resolution passes (RangeCheck) have already been applied
  // inside getGenericCircuit when the provsql.simplify_on_load GUC is
  // on (the default), so the circuit we receive here is already
  // peephole-pruned for any "always true / always false" comparator.
  GenericCircuit gc = getGenericCircuit(token);
  gate_t gc_root = gc.getGate(uuid2string(token));

  // Hybrid-evaluator simplifier: constant-fold gate_arith subtrees,
  // drop identity wires (0 from PLUS, 1 from TIMES), and collapse
  // PLUS over independent normals or i.i.d. exponentials into a
  // single gate_rv with the closed-form distribution.  Gated by
  // provsql.hybrid_evaluation (default on) so the unfolded DAG can
  // still be exercised end-to-end through the MC fallback during
  // A/B-testing.  Runs before AnalyticEvaluator so newly-bare normal
  // / Erlang leaves unlock the closed-form CDF on the surrounding
  // cmp gate.  Runs before a re-pass of RangeCheck so that the
  // joint-conjunction pass also benefits from constant folding
  // (e.g. a cmp's `arith(NEG, value:100)` operand becomes a bare
  // `value:-100` that the asRvVsConstCmp shape match accepts).
  if (provsql_hybrid_evaluation) {
    provsql::runHybridSimplifier(gc);
    provsql::runRangeCheck(gc);
  }

  // Probability-specific peephole: AnalyticEvaluator decides
  // continuous-RV comparators with closed-form CDFs (X cmp c for any
  // bare gate_rv leaf, X cmp Y for two independent normal leaves) by
  // replacing them with Bernoulli gate_input gates carrying the
  // analytical probability.  Always sound for probability evaluation;
  // produces fractional probabilities so it is meaningful only on
  // this path (not in getGenericCircuit, which is shared with
  // semiring evaluators).  Runs after RangeCheck so the cheaper
  // 0/1 decisions are already taken; AnalyticEvaluator only sees
  // the comparators RangeCheck could not collapse.
  provsql::runAnalyticEvaluator(gc);

  // Hybrid-evaluator island decomposer: per-cmp MC marginalisation
  // of the residual continuous-island comparators that none of the
  // earlier passes could resolve (e.g. heterogeneous distributions
  // under arith, or normal+uniform compositions outside the analytic
  // CDF's scope).  Each qualifying cmp becomes a Bernoulli gate_input
  // so the surrounding Boolean structure can be evaluated by every
  // existing method ('independent', 'tree-decomposition',
  // 'monte-carlo', 'd4', etc.) without the BoolExpr semiring choking
  // on a leftover gate_arith leaf.  Single-cmp islands only; cmps
  // that share a base RV with another unresolved cmp are skipped and
  // fall through to whole-circuit MC (handled later via monteCarloRV
  // when the method permits, or by the multi-cmp joint-table half
  // of Priority 7(b) once that lands).
  if (provsql_hybrid_evaluation) {
    provsql::runHybridDecomposer(
      gc, static_cast<unsigned>(provsql_rv_mc_samples));
  }

  double result;

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  try {
    // RV-aware Monte Carlo: when the circuit contains continuous
    // random-variable leaves, the BoolExpr translation in
    // getBooleanCircuit drops them, so we sample directly on the
    // GenericCircuit instead.  Other probability methods are not
    // (yet) defined over RV circuits.
    if(method == "monte-carlo" && provsql::circuitHasRV(gc, gc_root)) {
      int samples = 0;
      try {
        samples = stoi(args);
      } catch(const std::invalid_argument &) {
      }
      if(samples <= 0)
        provsql_error("Invalid number of samples: '%s'", args.c_str());

      result = provsql::monteCarloRV(gc, gc_root, samples);
    } else {
      // Existing Boolean-circuit path: applies HAVING semantics and
      // BoolExpr translation, then dispatches across the legacy
      // probability methods.
      gate_t gate;
      std::unordered_map<gate_t, gate_t> gc_to_bc;
      BooleanCircuit c = getBooleanCircuit(gc, token, gate, gc_to_bc);

      bool processed = false;

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
