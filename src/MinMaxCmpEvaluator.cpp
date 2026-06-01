/**
 * @file MinMaxCmpEvaluator.cpp
 * @brief Implementation of the MIN / MAX closed-form pre-pass.
 *        See @c MinMaxCmpEvaluator.h for the closed forms and the
 *        soundness contract.
 */
#include "MinMaxCmpEvaluator.h"

#include <vector>

#include "Aggregation.h"        // AggregationOperator + ComparisonOperator
#include "CmpEvaluatorCommon.h" // matchAggCmp, computeRefCounts, contributorProb

namespace provsql {

namespace {

/* ∏_{i : pred(m_i)} (1 - p_i) : the probability that every child whose
 * value satisfies @p pred is absent.  An empty product is 1.0. */
template <typename Pred>
static double qprod(const std::vector<double> &p,
                    const std::vector<long> &m,
                    Pred pred)
{
  double q = 1.0;
  for (std::size_t i = 0; i < p.size(); ++i)
    if (pred(m[i])) q *= (1.0 - p[i]);
  return q;
}

/* Pr(MIN/MAX = C) under HAVING semantics (empty group excluded).
 * MAX = C : no child > C present, at least one child = C present.
 * MIN = C : no child < C present, at least one child = C present. */
static double probEqual(AggregationOperator agg,
                        const std::vector<double> &p,
                        const std::vector<long> &m, int C)
{
  if (agg == AggregationOperator::MAX) {
    double no_above   = qprod(p, m, [&](long v) { return v > C; });
    double some_equal = 1.0 - qprod(p, m, [&](long v) { return v == C; });
    return no_above * some_equal;
  } else {  // MIN
    double no_below   = qprod(p, m, [&](long v) { return v < C; });
    double some_equal = 1.0 - qprod(p, m, [&](long v) { return v == C; });
    return no_below * some_equal;
  }
}

/* Closed-form Pr(agg(a) op C) over the independent contributor marginals
 * @p p with deterministic per-row values @p m.  Empty group excluded for
 * every operator (mirrors count_enum / CountCmpEvaluator). */
static double cdfForOperator(AggregationOperator agg,
                             ComparisonOperator op,
                             const std::vector<double> &p,
                             const std::vector<long> &m,
                             long C)
{
  auto atLeastOne = [&](auto pred) { return 1.0 - qprod(p, m, pred); };

  if (agg == AggregationOperator::MAX) {
    switch (op) {
      case ComparisonOperator::GE:
        return atLeastOne([&](long v) { return v >= C; });
      case ComparisonOperator::GT:
        return atLeastOne([&](long v) { return v >  C; });
      case ComparisonOperator::LE:
        return qprod(p, m, [&](long v) { return v >  C; })
             * atLeastOne([&](long v) { return v <= C; });
      case ComparisonOperator::LT:
        return qprod(p, m, [&](long v) { return v >= C; })
             * atLeastOne([&](long v) { return v <  C; });
      case ComparisonOperator::EQ:
        return probEqual(agg, p, m, C);
      case ComparisonOperator::NE:
        return (1.0 - qprod(p, m, [](long) { return true; }))
             - probEqual(agg, p, m, C);
    }
  } else {  // MIN
    switch (op) {
      case ComparisonOperator::LE:
        return atLeastOne([&](long v) { return v <= C; });
      case ComparisonOperator::LT:
        return atLeastOne([&](long v) { return v <  C; });
      case ComparisonOperator::GE:
        return qprod(p, m, [&](long v) { return v <  C; })
             * atLeastOne([&](long v) { return v >= C; });
      case ComparisonOperator::GT:
        return qprod(p, m, [&](long v) { return v <= C; })
             * atLeastOne([&](long v) { return v >  C; });
      case ComparisonOperator::EQ:
        return probEqual(agg, p, m, C);
      case ComparisonOperator::NE:
        return (1.0 - qprod(p, m, [](long) { return true; }))
             - probEqual(agg, p, m, C);
    }
  }
  return 0.0;
}

}  // namespace

unsigned runMinMaxCmpEvaluator(GenericCircuit &gc)
{
  unsigned resolved = 0;
  const auto nb = gc.getNbGates();

  /* Snapshot the cmp-gate ids so in-place rewrites don't affect the
   * iteration : same pattern as runCountCmpEvaluator. */
  std::vector<gate_t> cmps;
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) == gate_cmp)
      cmps.push_back(g);
  }
  if (cmps.empty()) return 0;

  /* Reference counts computed once; resolveCmpToBernoulli only clears
   * the cmp's own wires, leaving every other gate's ref count intact. */
  auto ref = computeRefCounts(gc);

  for (gate_t cmp : cmps) {
    if (gc.getGateType(cmp) != gate_cmp) continue;  /* defensive */

    AggCmpMatch match;
    if (!matchAggCmp(gc, cmp, match))
      continue;

    if (match.agg_kind != AggregationOperator::MIN &&
        match.agg_kind != AggregationOperator::MAX)
      continue;

    /* Independence certification, identical to runCountCmpEvaluator :
     * the aggregate is consumed by this cmp alone, each semimod by the
     * aggregate alone, and every randomness-bearing gate inside K_i has
     * reference count 1 (so the contributors are pairwise leaf-disjoint,
     * private to the cmp, and individually read-once).  See
     * CountCmpEvaluator.h for the full argument. */
    if (ref[static_cast<std::size_t>(match.agg)] != 1) continue;
    bool sound = true;
    std::vector<double> p;
    p.reserve(match.ks.size());
    for (std::size_t i = 0; i < match.ks.size(); ++i) {
      if (ref[static_cast<std::size_t>(match.semimods[i])] != 1) { sound = false; break; }
      double pi = contributorProb(gc, match.ks[i], ref, sound);
      if (!sound) break;
      p.push_back(pi);
    }
    if (!sound) continue;

    double pr = cdfForOperator(match.agg_kind, match.op, p, match.ms, match.C);

    /* Defensive clamp against floating-point roundoff. */
    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
