/**
 * @file CountCmpEvaluator.cpp
 * @brief Implementation of the Poisson-binomial pre-pass.
 *        See @c CountCmpEvaluator.h for the full docstring.
 */
#include "CountCmpEvaluator.h"

#include <algorithm>
#include <vector>

#include "Aggregation.h"          // ComparisonOperator + getAggregationOperator
#include "CmpEvaluatorCommon.h"   // matchAggCmp, computeRefCounts, contributorProb

namespace provsql {

namespace {

/* Partial Poisson-binomial PMF : compute @c dp[j] = Pr(exactly @c j
 * successes among the @c N input Bernoullis) for @c j in @c [0, jmax]
 * only.  @c jmax is clamped to @c N.  Cost : @c O(N x jmax).  Rolling
 * 1-D array, iterate j downward so each read references the
 * not-yet-updated previous row. */
static std::vector<double> partialPMF(const std::vector<double> &p,
                                      std::size_t jmax)
{
  const std::size_t N = p.size();
  jmax = std::min(jmax, N);
  std::vector<double> dp(jmax + 1, 0.0);
  dp[0] = 1.0;
  for (std::size_t i = 0; i < N; ++i) {
    const double pi = p[i];
    const double qi = 1.0 - pi;
    /* Cap inner loop at min(i+1, jmax) : entries beyond i are still
     * zero and entries beyond jmax we never sum. */
    const std::size_t upper = std::min(jmax, i + 1);
    for (std::size_t j = upper; j >= 1; --j) {
      dp[j] = dp[j] * qi + dp[j - 1] * pi;
    }
    dp[0] *= qi;
  }
  return dp;
}

/* Probability that the empty world occurs : @c prod_i (1 - p_i).
 * Always needed for SQL HAVING semantics (the empty group never
 * satisfies). */
static double probZero(const std::vector<double> &p)
{
  double q = 1.0;
  for (double pi : p) q *= (1.0 - pi);
  return q;
}

/* Probability that at least @c T of the @c N Bernoullis succeed.
 * Dispatches on which side of @c T is closer to the boundary to keep
 * the partial DP at @c O(N x min(T, N - T + 1)).
 *  - If @c T-1 <= N-T (lower tail is smaller) : compute the lower
 *    partial PMF up to @c T-1 and return @c 1 - sum.
 *  - Otherwise (upper tail is smaller) : invert the Bernoullis,
 *    @c Y_i = 1 - X_i, and use @c Pr(B >= T) = Pr(sum Y <= N - T) ;
 *    the partial PMF on @c Y is computed up to @c N - T. */
static double probAtLeast(const std::vector<double> &p, int T)
{
  const int N = static_cast<int>(p.size());
  if (T <= 0) return 1.0;
  if (T >  N) return 0.0;

  if (T - 1 <= N - T) {
    auto dp = partialPMF(p, static_cast<std::size_t>(T - 1));
    double sum = 0.0;
    for (int j = 0; j <= T - 1; ++j) sum += dp[j];
    return 1.0 - sum;
  } else {
    std::vector<double> q(N);
    for (int i = 0; i < N; ++i) q[i] = 1.0 - p[i];
    auto dp = partialPMF(q, static_cast<std::size_t>(N - T));
    double sum = 0.0;
    for (int j = 0; j <= N - T; ++j) sum += dp[j];
    return sum;
  }
}

/* Probability that at most @c T of the @c N Bernoullis succeed.
 * Same smaller-side dispatch as @c probAtLeast : if @c T is closer
 * to 0 compute the lower partial PMF and sum ; if @c T is closer to
 * @c N invert and compute the upper tail's complement. */
static double probAtMost(const std::vector<double> &p, int T)
{
  const int N = static_cast<int>(p.size());
  if (T <  0) return 0.0;
  if (T >= N) return 1.0;

  if (T <= N - 1 - T) {
    auto dp = partialPMF(p, static_cast<std::size_t>(T));
    double sum = 0.0;
    for (int j = 0; j <= T; ++j) sum += dp[j];
    return sum;
  } else {
    std::vector<double> q(N);
    for (int i = 0; i < N; ++i) q[i] = 1.0 - p[i];
    auto dp = partialPMF(q, static_cast<std::size_t>(N - 1 - T));
    double sum = 0.0;
    for (int j = 0; j <= N - 1 - T; ++j) sum += dp[j];
    return 1.0 - sum;
  }
}

/* Probability that exactly @c T of the @c N Bernoullis succeed.
 * Same smaller-side dispatch : @c Pr(B = T) = @c Pr(sum Y = N - T)
 * with @c Y_i = 1 - X_i, computed at whichever side has the smaller
 * partial PMF. */
static double probEqual(const std::vector<double> &p, int T)
{
  const int N = static_cast<int>(p.size());
  if (T < 0 || T > N) return 0.0;

  if (T <= N - T) {
    auto dp = partialPMF(p, static_cast<std::size_t>(T));
    return dp[T];
  } else {
    std::vector<double> q(N);
    for (int i = 0; i < N; ++i) q[i] = 1.0 - p[i];
    auto dp = partialPMF(q, static_cast<std::size_t>(N - T));
    return dp[N - T];
  }
}

/* Map operator + threshold to @c Pr(B op C) under SQL HAVING
 * semantics : the empty-group case (@c B = 0) is excluded regardless
 * of operator, matching @c count_enum's @c if (m < 1) m = 1 clamp
 * and its @c x >= 1 enumeration lower bound.
 *
 * Each branch picks at most two of probAtLeast / probAtMost /
 * probEqual / probZero, each O(N x min(C, N-C)) ; the whole
 * dispatch is therefore O(N x min(C, N-C)) per cmp. */
static double cdfForOperator(const std::vector<double> &p,
                             ComparisonOperator op,
                             int C)
{
  const int N = static_cast<int>(p.size());
  switch (op) {
    case ComparisonOperator::GE: {
      /* sizes >= max(C, 1) ; the clamp excludes the empty world for
       * GE 0 / GE -K cases.  No further pZero subtraction needed
       * because the [eff_lo, N] range starts at 1 or above. */
      return probAtLeast(p, std::max(C, 1));
    }
    case ComparisonOperator::GT: {
      return probAtLeast(p, std::max(C + 1, 1));
    }
    case ComparisonOperator::LE: {
      /* sizes [1, min(C, N)] = Pr(B <= min(C, N)) - Pr(B = 0). */
      const int T = std::min(C, N);
      if (T < 1) return 0.0;
      return probAtMost(p, T) - probZero(p);
    }
    case ComparisonOperator::LT: {
      const int T = std::min(C - 1, N);
      if (T < 1) return 0.0;
      return probAtMost(p, T) - probZero(p);
    }
    case ComparisonOperator::EQ: {
      if (C < 1 || C > N) return 0.0;
      return probEqual(p, C);
    }
    case ComparisonOperator::NE: {
      /* sizes [1, N] \ {C} = (1 - Pr(B = 0)) - (Pr(B = C) if 1<=C<=N). */
      const double nonempty = 1.0 - probZero(p);
      const double eq = (C >= 1 && C <= N) ? probEqual(p, C) : 0.0;
      return nonempty - eq;
    }
  }
  return 0.0;
}

}  // namespace

unsigned runCountCmpEvaluator(GenericCircuit &gc)
{
  unsigned resolved = 0;
  const auto nb = gc.getNbGates();

  /* Snapshot the cmp-gate ids so in-place rewrites don't affect the
   * iteration : same pattern as runAnalyticEvaluator. */
  std::vector<gate_t> cmps;
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) == gate_cmp)
      cmps.push_back(g);
  }
  if (cmps.empty()) return 0;

  /* Reference counts are computed once and not updated as we resolve
   * cmps : resolveCmpToBernoulli only clears the cmp's wires (it does
   * not touch any other gate), so children's ref counts are unchanged
   * with respect to the rest of the circuit.  The snapshot reflects
   * the pre-pass state, which is what we need to certify "no outside
   * reachability" for each candidate's input leaves. */
  auto ref = computeRefCounts(gc);

  for (gate_t cmp : cmps) {
    if (gc.getGateType(cmp) != gate_cmp) continue;  /* defensive */

    AggCmpMatch match;
    if (!matchAggCmp(gc, cmp, match))
      continue;

    /* COUNT(*) over unit-weighted contributors only.  matchAggCmp has
     * already remapped SUM-of-1s to COUNT; a genuine COUNT with a
     * non-unit weight, or a SUM / MIN / MAX / AVG aggregate, is out of
     * this pre-pass's scope and is left for its own evaluator or for
     * provsql_having. */
    if (match.agg_kind != AggregationOperator::COUNT) continue;
    {
      bool all_one = true;
      for (int m : match.ms) if (m != 1) { all_one = false; break; }
      if (!all_one) continue;
    }

    const gate_t agg = match.agg;
    const auto &semimods = match.semimods;
    const auto &ks = match.ks;
    const ComparisonOperator op = match.op;
    const int C = match.C;

    /* Independence certification.  The contributors are independent
     * Bernoulli trials -- the precondition for the Poisson-binomial --
     * exactly when each contributor's sub-circuit
     *
     *     K_i -> semimod_i -> gate_agg -> cmp
     *
     * is a private read-once tree, sharing no randomness with another
     * contributor or with the rest of the circuit.  We check:
     *
     *  1. ref_count[gate_agg] == 1 : the aggregate is consumed by this
     *     cmp alone (catches HAVING COUNT(*) >= a AND COUNT(*) <= b
     *     over a shared count, which would couple the cmps).
     *  2. ref_count[semimod_i] == 1 : the wrapper is consumed by
     *     gate_agg alone.
     *  3. Every randomness-bearing gate inside K_i has ref_count == 1
     *     (verified by @c contributorProb as it recurses).  A single
     *     condition that simultaneously gives: leaf sets pairwise
     *     disjoint across contributors (a shared gate would have
     *     ref >= 2), no reuse outside the cmp (an external parent would
     *     push ref >= 2), and read-once-ness within a contributor (a
     *     leaf used twice would have ref >= 2) -- so the contributor's
     *     marginal is its read-once probability and the contributors
     *     are mutually independent.  Generalises the previous
     *     "K_i is a single gate_input" rule to arbitrary products /
     *     sums of private leaves (e.g. the bid * expertise row of a
     *     join), and bails (leaving the cmp for provsql_having) on any
     *     unsupported gate type.
     *
     * Constants on the path (semimod's M = gate_value(1), the
     * const_side gate_one + gate_value(C), and any gate_one / gate_zero
     * inside a contributor) carry no randomness, so their ref counts
     * are not constrained. */
    if (ref[static_cast<std::size_t>(agg)] != 1) continue;
    bool sound = true;
    std::vector<double> p;
    p.reserve(ks.size());
    for (std::size_t i = 0; i < ks.size(); ++i) {
      if (ref[static_cast<std::size_t>(semimods[i])] != 1) { sound = false; break; }
      double pi = contributorProb(gc, ks[i], ref, sound);
      if (!sound) break;
      p.push_back(pi);
    }
    if (!sound) continue;

    /* Run the smaller-side dispatch over the contributor marginals. */
    double pr = cdfForOperator(p, op, C);

    /* Defensive clamp against floating-point roundoff. */
    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
