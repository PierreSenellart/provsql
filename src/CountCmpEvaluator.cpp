/**
 * @file CountCmpEvaluator.cpp
 * @brief Implementation of the Poisson-binomial pre-pass.
 *        See @c CountCmpEvaluator.h for the full docstring.
 */
#include "CountCmpEvaluator.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "Aggregation.h"        // ComparisonOperator + cmpOpFromOid + getAggregationOperator
#include "having_semantics.hpp" // extract_constant_C, semimod_extract_M_and_K, flip_op
extern "C" {
#include "provsql_utils.h"      // gate_type enum
}

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

/* Try to match @c cmp against the first-slice scope.  On success,
 * fill @p agg_out (the gate_agg child, exposed to the caller for the
 * downstream "no shared gate_agg across cmps" check), @p children
 * (the K side of each semimod, after verifying it is a single
 * @c gate_input), @p op (already flipped if the agg sits on the
 * right), and @p C.  Returns @c false (and leaves outputs untouched)
 * for any shape mismatch.  Cheap to call : no allocation beyond the
 * @p children push_back. */
static bool matchCountCmp(GenericCircuit &gc,
                          gate_t cmp,
                          gate_t &agg_out,
                          std::vector<gate_t> &semimods_out,
                          std::vector<gate_t> &children,
                          ComparisonOperator &op,
                          int &C)
{
  const auto &cw = gc.getWires(cmp);
  if (cw.size() != 2) return false;

  bool okop = false;
  op = provsql_having_detail::map_cmp_op(gc, cmp, okop);
  if (!okop) return false;

  /* Identify which side is the gate_agg and which is the constant
   * wrapper.  Mirror collect_sp_cmp_gates : both orderings are
   * legitimate (R compared to L, or L compared to R), and the second
   * case calls for op flipping. */
  gate_t agg_side = cw[0], const_side = cw[1];
  if (gc.getGateType(agg_side) != gate_agg ||
      !provsql_having_detail::extract_constant_C(gc, const_side, C)) {
    agg_side = cw[1]; const_side = cw[0];
    if (gc.getGateType(agg_side) != gate_agg ||
        !provsql_having_detail::extract_constant_C(gc, const_side, C)) {
      return false;
    }
    op = provsql_having_detail::flip_op(op);
  }

  /* Aggregation must be COUNT, either directly or via the SUM-of-1s
   * encoding the planner emits for COUNT(*).  Mirror the dispatch in
   * pw_from_cmp_gate's build_from. */
  AggregationOperator agg_kind =
    getAggregationOperator(gc.getInfos(agg_side).first);
  if (agg_kind != AggregationOperator::COUNT &&
      agg_kind != AggregationOperator::SUM) {
    return false;
  }

  const auto &agg_children = gc.getWires(agg_side);
  if (agg_children.empty()) return false;

  /* Side-channel the semimod parents back to the caller so it can
   * check their ref counts ; the chain k_i -> semimod -> gate_agg
   * is the path the soundness argument follows up to cmp. */
  semimods_out.clear();
  semimods_out.reserve(agg_children.size());

  std::vector<gate_t> ks;
  ks.reserve(agg_children.size());

  for (gate_t ch : agg_children) {
    if (gc.getGateType(ch) != gate_semimod) return false;
    int m = 0;
    gate_t k_gate{};
    if (!provsql_having_detail::semimod_extract_M_and_K(gc, ch, m, k_gate))
      return false;
    /* COUNT(*) requires unit weights ; under the SUM encoding any
     * non-unit weight means the aggregate is a real SUM and this
     * pre-pass should not fire. */
    if (m != 1) return false;
    if (gc.getGateType(k_gate) != gate_input) return false;
    semimods_out.push_back(ch);
    ks.push_back(k_gate);
  }

  agg_out = agg_side;
  children = std::move(ks);
  return true;
}

/* Compute the reference count of every gate as a wire-target across
 * the whole circuit.  One pass over all gates' wire lists ;
 * @c O(total wires) time, @c O(nb_gates) space. */
static std::vector<unsigned> computeRefCounts(const GenericCircuit &gc)
{
  const auto nb = gc.getNbGates();
  std::vector<unsigned> ref(nb, 0);
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    for (gate_t w : gc.getWires(g)) {
      const auto idx = static_cast<std::size_t>(w);
      if (idx < ref.size()) ++ref[idx];
    }
  }
  return ref;
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

    gate_t agg{};
    std::vector<gate_t> semimods, ks;
    ComparisonOperator op;
    int C = 0;
    if (!matchCountCmp(gc, cmp, agg, semimods, ks, op, C))
      continue;

    /* Independence certification.  The soundness condition we want
     * is "the cmp's input leaves K_i appear nowhere else in the
     * circuit" ; equivalently, the chain
     *
     *     K_i -> semimod_i -> gate_agg -> cmp
     *
     * must be private to this cmp.  Checking ref_count == 1 at every
     * link along that chain (other than cmp itself, which is the
     * gate we are replacing) is sufficient :
     *
     *  1. ref_count[gate_agg] == 1 : the aggregate is consumed by
     *     this cmp alone (catches HAVING COUNT(*) >= a AND
     *     COUNT(*) <= b style multi-cmp expressions over a shared
     *     count, which would couple the two cmps through the agg).
     *  2. ref_count[semimod_i] == 1 : the wrapper is consumed by
     *     gate_agg alone (catches the unusual case of a cached
     *     semimod shared with something outside this cmp).
     *  3. ref_count[K_i] == 1 : the leaf is consumed by its
     *     wrapping semimod alone (catches K_i appearing in any
     *     other part of the circuit, in particular other cmps over
     *     the same row).
     *  4. The K_i's are pairwise distinct (catches the same leaf
     *     appearing twice in the same agg via two different
     *     semimods, which would still be inside the subtree but
     *     would double-count the row).
     *
     * Constants on the path (semimod's M = gate_value(1), the
     * const_side semimod's gate_one + gate_value(C)) carry no
     * randomness, so their ref counts are irrelevant. */
    if (ref[static_cast<std::size_t>(agg)] != 1) continue;
    std::unordered_set<gate_t> seen;
    bool sound = true;
    for (std::size_t i = 0; i < ks.size(); ++i) {
      if (ref[static_cast<std::size_t>(semimods[i])] != 1) { sound = false; break; }
      if (ref[static_cast<std::size_t>(ks[i])] != 1)       { sound = false; break; }
      if (!seen.insert(ks[i]).second)                      { sound = false; break; }
    }
    if (!sound) continue;

    /* Gather marginals and run the smaller-side dispatch. */
    std::vector<double> p;
    p.reserve(ks.size());
    for (gate_t k : ks) p.push_back(gc.getProb(k));

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
