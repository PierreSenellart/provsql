/**
 * @file ComparatorResolution.cpp
 * @brief Definition of @c resolveComparators (the shared comparator-
 *        resolution pipeline).  @c booleanSubcircuitProbability is defined
 *        in @c probability_evaluate.cpp, where the method-dispatch
 *        machinery (@c EvalContext / @c MethodCatalog) lives.
 */
#include "ComparatorResolution.h"

#include "AggMarginalEvaluator.h"
#include "AnalyticEvaluator.h"
#include "CountCmpEvaluator.h"
#include "HybridEvaluator.h"
#include "MinMaxCmpEvaluator.h"
#include "RangeCheck.h"
#include "SumCmpEvaluator.h"

#include <set>
#include <stack>
#include <string>
#include <vector>

extern "C" {
#include "provsql_utils.h"
#include "provsql_error.h"
}

namespace provsql {

namespace {
/// Gates reachable from @p r (for the pre-pass reduction NOTICE).
std::size_t count_reachable(const GenericCircuit &gc, gate_t r)
{
  std::set<gate_t> seen;
  std::stack<gate_t> stk;
  stk.push(r);
  while (!stk.empty()) {
    gate_t g = stk.top(); stk.pop();
    if (!seen.insert(g).second) continue;
    for (gate_t c : gc.getWires(g)) stk.push(c);
  }
  return seen.size();
}
}  // namespace

void resolveComparators(GenericCircuit &gc, gate_t root,
                        bool simplify, bool decompose)
{
  // Hybrid-evaluator value simplifier: constant-fold gate_arith, drop
  // identity wires, collapse PLUS over independent RVs into a closed-form
  // gate_rv.  Runs before AnalyticEvaluator so newly-bare leaves unlock the
  // closed-form CDF on the surrounding cmp, and before a re-pass of
  // RangeCheck so the joint-conjunction pass benefits from constant folding.
  // See the header for why the moment path passes simplify == false.
  if (simplify && provsql_hybrid_evaluation) {
    runHybridSimplifier(gc);
    runRangeCheck(gc);
  }

  // Island decomposer: group continuous-island comparators by base-RV
  // footprint overlap and inline a joint-distribution table (a gate_plus
  // over gate_mulinputs) for each shared group, so correlated comparators
  // stay correlation-aware through the Boolean translation.  Runs before
  // AnalyticEvaluator, which would otherwise resolve each shared comparator
  // into an independent marginal Bernoulli.  See the header for why the
  // moment path passes decompose == false.
  if (decompose && provsql_hybrid_evaluation)
    runHybridDecomposer(gc, static_cast<unsigned>(provsql_rv_mc_samples));

  const std::size_t gates_before = count_reachable(gc, root);

  // AnalyticEvaluator decides residual continuous-RV comparators (singleton
  // bare gate_rv vs gate_value, or two bare normals) by closed-form CDF,
  // replacing each with a Bernoulli gate_input carrying the probability.
  const unsigned analytic = runAnalyticEvaluator(gc);

  // Closed-form HAVING cmp-probability evaluators: Poisson-binomial for
  // COUNT(*) op C, the MIN / MAX closed forms, the weighted-sum DP for
  // SUM(a) op C, and the hierarchical marginal-vector engine for the safe
  // join shapes the flat pre-passes cannot certify independent.
  unsigned count_cmp = 0, minmax = 0, sum = 0, agg_marginal = 0;
  if (provsql_cmp_probability_evaluation) {
    count_cmp     = runCountCmpEvaluator(gc);
    minmax        = runMinMaxCmpEvaluator(gc);
    sum           = runSumCmpEvaluator(gc);
    agg_marginal  = runAggMarginalEvaluator(gc);
  }

  // Always-true HAVING rewrite (COUNT <= K with K >= N and duals): the sound
  // TRUE-decision arm RangeCheck deliberately leaves undone.
  const unsigned always_true = runHavingAlwaysTrueRewriter(gc);

  const unsigned total =
    analytic + count_cmp + minmax + sum + agg_marginal + always_true;
  if (total > 0 && provsql_verbose >= 5) {
    const std::size_t gates_after = count_reachable(gc, root);
    std::vector<std::string> parts;
    if (analytic > 0)     parts.push_back(std::to_string(analytic) + " analytic");
    if (count_cmp > 0)    parts.push_back(std::to_string(count_cmp) + " Poisson-binomial");
    if (minmax > 0)       parts.push_back(std::to_string(minmax) + " min/max");
    if (sum > 0)          parts.push_back(std::to_string(sum) + " weighted-sum");
    if (agg_marginal > 0) parts.push_back(std::to_string(agg_marginal) + " safe-join aggregate");
    if (always_true > 0)  parts.push_back(std::to_string(always_true) + " always-true");
    std::string breakdown;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) breakdown += " + ";
      breakdown += parts[i];
    }
    provsql_notice(
      "gate_cmp expression was shortcut by probability-side pre-pass "
      "(%s): provenance circuit reduced from %zu to %zu gates",
      breakdown.c_str(), gates_before, gates_after);
  }
}

}  // namespace provsql
