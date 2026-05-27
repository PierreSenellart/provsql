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
 * - @c "inversion-free": exact via the structured-d-DNNF builder over the
 *   query-derived order; errors unless the root carries an inversion-free
 *   certificate.  The default method also tries it (after @c "independent")
 *   when a certificate is present.
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
#include <stack>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <csignal>

#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "GenericCircuit.h"
#include "AnalyticEvaluator.h"
#include "CountCmpEvaluator.h"
#include "HybridEvaluator.h"
#include "RangeCheck.h"
#include "MonteCarloSampler.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "StructuredDNNF.h"
#include "having_semantics.hpp"
#include "provsql_mmap.h"
#include "safe_query_cert.h"
#include "provsql_utils_cpp.h"
#include "tool_registry_sync.h"
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
 * being silently absorbed.  (The matching case where an external
 * compiler is running is handled in @c run_external_tool, which runs the
 * tool in its own process group and @c SIGKILLs that group on a pending
 * cancel, then lets @c CHECK_FOR_INTERRUPTS raise it.)
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
 * @brief Collect the inversion-free per-input order keys for the structured
 *        builder.
 *
 * Walks the @c GenericCircuit @p gc for @c K-prefixed annotation gates whose
 * child is a @c gate_input (the per-input order markers attached by the planner
 * on the certified path), parses each key, and maps the wrapped input to its
 * @c BooleanCircuit variable via @p gc_to_bc.  Returns @c true only if every
 * @c BooleanCircuit input reachable from @p bc_root carries a key (the
 * structured builder needs a total order over all variables); a missing marker
 * means the certified markers are absent / incomplete and the caller must not
 * use the structured path.
 */
static bool collect_inversion_free_keys(
  const GenericCircuit &gc, gate_t gc_root,
  const std::unordered_map<gate_t, gate_t> &gc_to_bc,
  const BooleanCircuit &c, gate_t bc_root,
  std::map<gate_t, StructuredDNNFBuilder::InputKey> &out)
{
  std::set<gate_t> seen;
  std::stack<gate_t> st;
  st.push(gc_root);
  while (!st.empty()) {
    gate_t g = st.top(); st.pop();
    if (!seen.insert(g).second) continue;
    if (gc.getGateType(g) == gate_annotation) {
      std::string ex = gc.getExtra(g);   // must outlive the parse (k points into it)
      SafeCertKey k;
      if (safe_cert_key_parse(ex.c_str(), &k)) {
        const auto &w = gc.getWires(g);
        if (!w.empty() && gc.getGateType(w[0]) == gate_input) {
          auto it = gc_to_bc.find(w[0]);
          if (it != gc_to_bc.end())
            out[it->second] = StructuredDNNFBuilder::InputKey{
              std::string(k.root, k.root_len),
              std::string(k.sec, k.sec_len), k.factor };
        }
      }
    }
    for (gate_t ch : gc.getWires(g)) st.push(ch);
  }

  /* every Boolean input must be ordered */
  std::set<gate_t> bseen;
  std::stack<gate_t> bst;
  bst.push(bc_root);
  while (!bst.empty()) {
    gate_t g = bst.top(); bst.pop();
    if (!bseen.insert(g).second) continue;
    if (c.getGateType(g) == BooleanGate::IN) {
      if (out.find(g) == out.end())
        return false;
    } else {
      for (gate_t ch : c.getWires(g)) bst.push(ch);
    }
  }
  return true;
}

/**
 * @brief Flatten the per-input order keys into a total rank for the structured
 *        builder's order-only constructor.
 *
 * Sorts the certified inputs into a Prop. 4.5-consistent order -- root-class
 * value first (one independent block per value), then secondary-class value
 * (one tile per value within a block), then the shared self-join guard before
 * the payloads of its tile, then by factor -- and assigns consecutive ranks.
 * Ties (two inputs with identical keys) keep a deterministic order via the
 * input gate id, so distinct variables always get distinct ranks.  Unlike the
 * keyed (factored-sweep) constructor this makes no single-secondary-axis /
 * one-payload-per-tile assumption, so it certifies every hierarchical
 * inversion-free lineage, including the self-join-free case.
 */
static std::map<gate_t, int> inversion_free_rank(
  const std::map<gate_t, StructuredDNNFBuilder::InputKey> &keys)
{
  std::vector<std::pair<gate_t, StructuredDNNFBuilder::InputKey>> v(
    keys.begin(), keys.end());
  std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
    const auto &ka = a.second, &kb = b.second;
    if (ka.root != kb.root) return ka.root < kb.root;
    if (ka.sec  != kb.sec)  return ka.sec  < kb.sec;
    int ga = (ka.factor == StructuredDNNFBuilder::GUARD_FACTOR) ? 0 : 1;
    int gb = (kb.factor == StructuredDNNFBuilder::GUARD_FACTOR) ? 0 : 1;
    if (ga != gb) return ga < gb;
    if (ka.factor != kb.factor) return ka.factor < kb.factor;
    return a.first < b.first;
  });
  std::map<gate_t, int> rank;
  int r = 0;
  for (const auto &p : v) rank[p.first] = r++;
  return rank;
}

dDNNF buildInversionFreeDDNNF(pg_uuid_t token)
{
  // Compile a query certified inversion-free to its structured d-DNNF (the
  // same artefact the 'inversion-free' probability method builds), so the KC
  // surface can render / measure it.  Mirrors the dispatch in
  // probability_evaluate_internal: the per-input order keys live on the
  // GenericCircuit's annotation markers, so we go through the generic circuit
  // rather than getBooleanCircuit(token, ...) directly.
  GenericCircuit gc = getGenericCircuit(token);
  gate_t gc_root = gc.getGate(uuid2string(token));
  std::string ex = gc.getExtra(gc_root);
  if (ex.empty() || ex[0] != SAFE_CERT_EXTRA_PREFIX_RECIPE)
    throw CircuitException("compile 'inversion-free': the provenance root "
                           "carries no inversion-free certificate");
  gate_t root;
  std::unordered_map<gate_t, gate_t> gc_to_bc;
  BooleanCircuit c = getBooleanCircuit(gc, token, root, gc_to_bc);
  std::map<gate_t, StructuredDNNFBuilder::InputKey> keys;
  if (!collect_inversion_free_keys(gc, gc_root, gc_to_bc, c, root, keys))
    throw CircuitException("compile 'inversion-free': the certificate's inputs "
                           "lack per-input order markers");
  return StructuredDNNFBuilder(c, root, inversion_free_rank(keys)).dnnf();
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

  // Inversion-free tractability certificate: the planner wraps the per-row
  // provenance root in a transparent annotation gate carrying the serialised
  // SafeCert recipe.  Its presence routes the default probability chain through
  // the structured-d-DNNF builder (after independentEvaluation, before
  // tree-decomposition) and is required by the explicit 'inversion-free'
  // method.  The recipe is read here (early, before the simplifier passes); the
  // per-input order keys are collected at the dispatch point, where the
  // GenericCircuit->BooleanCircuit mapping is available.
  bool inv_free_cert = false;
  {
    std::string ex = gc.getExtra(gc_root);
    if (!ex.empty() && ex[0] == SAFE_CERT_EXTRA_PREFIX_RECIPE) {
      SafeCert *cert = safe_cert_parse(ex.c_str());
      if (cert != nullptr && cert->kind == CERT_INVERSION_FREE) {
        inv_free_cert = true;
        // Internal per-evaluation diagnostic (the certificate round-trips from
        // the planner), not a result-comprehension message: keep it at the
        // detector's debug-trace level (>= 30) so it stays out of the level-5
        // floor the Studio eval strip applies.
        if (provsql_verbose >= 30)
          provsql_notice("inversion-free certificate read back from circuit "
                         "root: %d atoms, %d classes, root_class=%d",
                         cert->natoms, cert->nclasses, cert->root_class);
      }
    }
  }

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

  // Hybrid-evaluator island decomposer: handles continuous-island
  // comparators by grouping them via base-RV footprint overlap.
  // Multi-cmp shared-island groups get a joint-distribution table
  // inlined as a mulinput block - via the monotone-shared-scalar
  // fast path (k+1 mulinputs; interval probabilities exact via
  // cdfAt when the shared scalar is a bare gate_rv, MC binning over
  // k+1 intervals when it is a gate_arith composite) when all cmps
  // share an lhs gate_t and have gate_value rhs, falling through to
  // the generic 2^k MC joint table otherwise.  Singleton bare-RV
  // cmps are left for AnalyticEvaluator (closed-form CDF on its own
  // is cheaper than per-cmp MC); singleton gate_arith cmps get a
  // per-cmp MC marginalisation here.  Either way the rewritten
  // cmps become gate_plus over mulinputs (or gate_input
  // Bernoullis), so the surrounding circuit is purely Boolean for
  // the downstream pass.
  //
  // Runs BEFORE AnalyticEvaluator so shared bare-RV cmps reach the
  // grouping logic - AnalyticEvaluator would otherwise resolve each
  // independently into a Bernoulli, silently using the independence
  // approximation on shared base RVs.  The trade-off: the fast
  // path's mulinput block is a dependent circuit that
  // BooleanCircuit::independentEvaluation rejects when the cmps
  // combine via AND ('Not an independent circuit').  Callers that
  // need shared-island dependence handling must use
  // 'tree-decomposition' / 'monte-carlo' / external compilation;
  // 'independent' remains correct only for circuits that ARE
  // independent.
  if (provsql_hybrid_evaluation) {
    provsql::runHybridDecomposer(
      gc, static_cast<unsigned>(provsql_rv_mc_samples));
  }

  // Probability-specific peephole: AnalyticEvaluator decides any
  // residual continuous-RV comparators the decomposer left alone
  // (singleton bare gate_rv vs gate_value, or two bare normals) by
  // replacing them with Bernoulli gate_input gates carrying the
  // analytical probability.  Always sound for probability
  // evaluation; produces fractional probabilities so it is
  // meaningful only on this path (not in getGenericCircuit, which
  // is shared with semiring evaluators).
  // Count gates reachable from the root before / after the
  // probability-side pre-pass, so the user can see how much the
  // shortcut shrank the circuit the downstream method actually sees.
  auto count_reachable = [&](gate_t r) {
    std::set<gate_t> seen;
    std::stack<gate_t> stk;
    stk.push(r);
    while (!stk.empty()) {
      gate_t g = stk.top(); stk.pop();
      if (!seen.insert(g).second) continue;
      for (gate_t c : gc.getWires(g)) stk.push(c);
    }
    return seen.size();
  };
  size_t gates_before = count_reachable(gc_root);
  unsigned analytic_resolved = provsql::runAnalyticEvaluator(gc);

  /* Closed-form cmp-probability evaluators.  First implementation
   * is the Poisson-binomial pre-pass for HAVING COUNT(*) op C over
   * distinct gate_input leaves ; future MIN / MAX / SUM resolvers
   * will run from the same hook.  Each replaces a matched gate_cmp
   * with a Bernoulli gate_input carrying the closed-form probability,
   * so the surrounding circuit can skip the DNF that
   * provsql_having's enumerate_valid_worlds would otherwise build.
   * Same probability-side sound-only caveat as runAnalyticEvaluator :
   * the gate_input carries a fractional probability so the rewrite
   * lives here, not in getGenericCircuit.  Hidden behind
   * provsql.cmp_probability_evaluation for developer A/B testing ;
   * on by default. */
  unsigned count_cmp_resolved = 0;
  if (provsql_cmp_probability_evaluation) {
    count_cmp_resolved = provsql::runCountCmpEvaluator(gc);
  }

  /* If either probability-side pre-pass replaced any cmp with a
   * closed-form Bernoulli, the formula the downstream tool sees is
   * not the formula the user wrote: it has had part (or all) of its
   * comparison structure folded into Bernoulli leaves before any
   * d-DNNF compiler / weighted model counter is invoked. Emit a
   * NOTICE (gated on provsql.verbose_level >= 5) so the user knows
   * the requested method's reported timing and structure may not
   * reflect work on the original formula. */
  if (analytic_resolved + count_cmp_resolved > 0 && provsql_verbose >= 5) {
    size_t gates_after = count_reachable(gc_root);
    std::string breakdown;
    if (analytic_resolved > 0 && count_cmp_resolved > 0) {
      breakdown = std::to_string(analytic_resolved) + " analytic + "
                + std::to_string(count_cmp_resolved) + " Poisson-binomial";
    } else if (analytic_resolved > 0) {
      breakdown = std::to_string(analytic_resolved) + " analytic";
    } else {
      breakdown = std::to_string(count_cmp_resolved) + " Poisson-binomial";
    }
    provsql_notice(
      "gate_cmp expression was shortcut by probability-side pre-pass "
      "(%s): provenance circuit reduced from %zu to %zu gates",
      breakdown.c_str(), gates_before, gates_after);
  }

  /* After every resolution pass has run, any gate_rv left in the
   * circuit reaches the BoolExpr translation in getBooleanCircuit
   * unchanged; that walk recurses into the surrounding gate_cmp and
   * calls semiring.value() on the gate_value side, producing the
   * generic "This semiring does not support value gates." error.
   * Detect that here and raise a message that names the actual
   * root cause: the analytical evaluators couldn't fold the RV
   * leaves away, and the MC fallback that would have decided the
   * surrounding cmp is either disabled (rv_mc_samples = 0) or
   * wasn't able to close the gap.  HAVING-style cmps over gate_agg
   * don't contain gate_rv, so this check leaves them for
   * provsql_having. */
  if (method != "monte-carlo" && provsql::circuitHasRV(gc, gc_root)) {
    if (provsql_rv_mc_samples <= 0) {
      provsql_error(
        "probability_evaluate: a comparison over random variables "
        "could not be resolved analytically; raise "
        "provsql.rv_mc_samples above 0 to enable the Monte Carlo "
        "fallback, or call probability_evaluate(..., 'monte-carlo', "
        "<n>) directly");
    } else {
      provsql_error(
        "probability_evaluate: a comparison over random variables "
        "could not be resolved analytically and the hybrid evaluator "
        "left it unresolved; call probability_evaluate(..., "
        "'monte-carlo', <n>) directly for an MC estimate");
    }
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
      } else if(method=="inversion-free") {
        // Explicit inversion-free method: requires the certificate, errors
        // otherwise (and ignores the provsql.inversion_free kill-switch).  The
        // structured builder throws on multivalued inputs (BID) and on a
        // variable left unordered by collect_inversion_free_keys.
        if(!inv_free_cert)
          provsql_error("method 'inversion-free' requires an inversion-free "
                        "certificate on the provenance root");
        std::map<gate_t, StructuredDNNFBuilder::InputKey> keys;
        if(!collect_inversion_free_keys(gc, gc_root, gc_to_bc, c, gate, keys))
          provsql_error("method 'inversion-free': the provenance root carries "
                        "a certificate but its inputs lack per-input order "
                        "markers");
        result = StructuredDNNFBuilder(c, gate, inversion_free_rank(keys))
                   .probability();
        processed = true;
      } else if(method=="") {
        // Default evaluation: independent, then (when an inversion-free
        // certificate is present) the structured-d-DNNF builder, then
        // tree-decomposition / compilation, in order until one works.
        try {
          result = c.independentEvaluation(gate);
          processed = true;
        } catch(CircuitException &) {}

        if(!processed && inv_free_cert && provsql_inversion_free) {
          try {
            std::map<gate_t, StructuredDNNFBuilder::InputKey> keys;
            if(collect_inversion_free_keys(gc, gc_root, gc_to_bc, c, gate, keys)) {
              result = StructuredDNNFBuilder(c, gate, inversion_free_rank(keys))
                         .probability();
              processed = true;
            }
          } catch(CircuitException &) {}   // fall through to tree-decomposition
        }
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
          // Backward-compatible alias for the wmc/weightmc path.
          result = c.wmcCount(gate, "weightmc", args);
        } else if(method=="wmc") {
          // 'args' = "tool[;tool_args]". 'tool' selects which weighted model
          // counter to invoke (any registered wmc tool); 'tool_args'
          // (optional, ';'-prefixed) is forwarded.  wmcCount validates the
          // tool against the registry, so there is no per-tool branch here.
          auto sep = args.find(';');
          std::string tool = (sep == std::string::npos) ? args : args.substr(0, sep);
          std::string tool_args = (sep == std::string::npos) ? std::string() : args.substr(sep + 1);
          result = c.wmcCount(gate, tool, tool_args);
        } else if(method=="compilation" || method=="tree-decomposition"
                  || method=="interpret-as-dd" || method=="default"
                  || method=="") {
          auto dd = c.makeDD(gate,
                             method=="default" ? std::string() : method,
                             args);
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
  provsql_sync_tool_registry();  // honour persisted tool-registry overrides
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
