/**
 * @file SumCmpEvaluator.cpp
 * @brief Implementation of the weighted-sum DP pre-pass.
 *        See @c SumCmpEvaluator.h for the DP, the soundness contract,
 *        and the pseudo-polynomial caveat.
 */
#include "SumCmpEvaluator.h"

#include <cstdint>
#include <vector>

#include "Aggregation.h"        // AggregationOperator + ComparisonOperator
#include "CmpEvaluatorCommon.h" // matchAggCmp, computeRefCounts, contributorProb

namespace provsql {

namespace {

/* Reachable-sum range cap.  The DP is O(N x R) in time and O(R) in
 * memory with R the reachable-sum span ; R is linear in the weight /
 * threshold magnitude, hence exponential in their bit-length (Remark 3
 * of the HAVING-complexity theory).  Above this span the pass declines
 * and the cmp falls back to the general enumeration path.  ~16M doubles
 * = 128 MB is the ceiling we accept for the closed form. */
constexpr long kMaxSumRange = 1L << 24;

/* Does the integer sum @p s satisfy @p s op C ? */
static bool sumSatisfies(long s, ComparisonOperator op, long C)
{
  switch (op) {
    case ComparisonOperator::EQ: return s == C;
    case ComparisonOperator::NE: return s != C;
    case ComparisonOperator::LE: return s <= C;
    case ComparisonOperator::LT: return s <  C;
    case ComparisonOperator::GE: return s >= C;
    case ComparisonOperator::GT: return s >  C;
  }
  return false;
}

}  // namespace

unsigned runSumCmpEvaluator(GenericCircuit &gc)
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

    /* Genuine SUM only.  matchAggCmp remaps SUM-of-1s (COUNT(*)) to
     * COUNT, so a match that is still SUM here carries real weights;
     * MIN / MAX / AVG are out of this pass's scope. */
    if (match.agg_kind != AggregationOperator::SUM)
      continue;

    /* Reachable-sum range [lo, hi] from the weights, and the cap check
     * (done before the independence walk : a too-wide range is rejected
     * regardless of soundness). */
    long lo = 0, hi = 0;
    for (int m : match.ms) {
      if (m < 0) lo += m; else hi += m;
    }
    const long range = hi - lo + 1;
    if (range <= 0 || range > kMaxSumRange) continue;

    /* Independence certification, identical to runCountCmpEvaluator. */
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

    /* Subset-sum DP : dp[s - lo] = Pr(sum of present weights = s).
     * Start from the empty world (sum 0, mass 1), fold in each child. */
    std::vector<double> dp(static_cast<std::size_t>(range), 0.0);
    dp[static_cast<std::size_t>(-lo)] = 1.0;  /* sum 0 */
    for (std::size_t i = 0; i < p.size(); ++i) {
      const long m = match.ms[i];
      const double pi = p[i];
      const double qi = 1.0 - pi;
      if (m == 0) {
        /* Present or absent leaves the sum unchanged ; the row only
         * affects group (non-)emptiness, handled by the empty-world
         * subtraction below.  Mass is conserved, so dp is untouched. */
        continue;
      }
      std::vector<double> ndp(static_cast<std::size_t>(range), 0.0);
      for (long s = lo; s <= hi; ++s) {
        const double cur = dp[static_cast<std::size_t>(s - lo)];
        if (cur == 0.0) continue;
        /* Child absent : sum stays s. */
        ndp[static_cast<std::size_t>(s - lo)] += cur * qi;
        /* Child present : sum becomes s + m (always in range by
         * construction of [lo, hi]). */
        ndp[static_cast<std::size_t>(s + m - lo)] += cur * pi;
      }
      dp.swap(ndp);
    }

    /* Naive Pr(S op C) over all worlds, then exclude the empty group. */
    double pr = 0.0;
    for (long s = lo; s <= hi; ++s) {
      if (sumSatisfies(s, match.op, match.C))
        pr += dp[static_cast<std::size_t>(s - lo)];
    }
    if (sumSatisfies(0, match.op, match.C)) {
      double prEmpty = 1.0;
      for (double pi : p) prEmpty *= (1.0 - pi);
      pr -= prEmpty;
    }

    /* Defensive clamp against floating-point roundoff. */
    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
