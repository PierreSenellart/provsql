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
 * (the K side of each semimod : the per-row contributor sub-circuit
 * root, single leaf or product), @p op (already flipped if the agg
 * sits on the right), and @p C.  Returns @c false (and leaves outputs untouched)
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
    /* The K side is the per-row contributor sub-circuit root.  It need
     * not be a single gate_input : a SELECT-FROM-WHERE group row is
     * typically a @c times of the joined tuples' leaves.  The caller
     * certifies independence and computes the contributor's marginal. */
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

/* Marginal probability of one count contributor (the K side of a
 * semimod), computed by a read-once recursion over the contributor's
 * sub-circuit.  This is exact precisely when the sub-circuit is a
 * private read-once tree, which is what the caller certifies: every
 * structural (randomness-bearing) gate it visits must have reference
 * count 1, so it has a single parent inside this contributor and is
 * shared neither with another contributor nor with the rest of the
 * circuit.  That single condition gives all three properties the
 * Poisson-binomial needs -- pairwise-disjoint leaf sets across
 * contributors, no reuse outside the cmp, and read-once-ness within a
 * contributor -- so the contributors are independent and each marginal
 * is its read-once probability.
 *
 * Supported gate types are the ones a SELECT-FROM-WHERE (-EXCEPT) group
 * row can carry: @c input (Bernoulli leaf), @c times (independent AND),
 * @c plus (independent OR over alternative derivations), @c monus
 * (@c a @c AND @c NOT @c b, i.e. Pr(a)(1-Pr(b)) for independent a, b),
 * and the constants @c one / @c zero.  Constants bear no randomness, so
 * their reference counts are not constrained (they may be
 * cached/shared).  Any other gate type (@c value, @c mulinput, @c agg,
 * @c cmp, @c rv, ...) means the contributor is outside this pre-pass's
 * scope; @p ok is cleared and the caller skips the whole cmp. */
static double contributorProb(const GenericCircuit &gc, gate_t g,
                              const std::vector<unsigned> &ref, bool &ok)
{
  switch (gc.getGateType(g)) {
    case gate_one:  return 1.0;
    case gate_zero: return 0.0;
    case gate_input:
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      return gc.getProb(g);
    case gate_times: {
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      double pr = 1.0;
      for (gate_t c : gc.getWires(g)) {
        pr *= contributorProb(gc, c, ref, ok);
        if (!ok) return 0.0;
      }
      return pr;
    }
    case gate_plus: {
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      double q = 1.0;
      for (gate_t c : gc.getWires(g)) {
        q *= (1.0 - contributorProb(gc, c, ref, ok));
        if (!ok) return 0.0;
      }
      return 1.0 - q;
    }
    case gate_monus: {
      /* a (-) b = a AND NOT b ; with disjoint private leaves a and b
       * are independent, so Pr = Pr(a) * (1 - Pr(b)).  Children are
       * [minuend, subtrahend] (see GenericCircuit evaluate<S>). */
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      const auto &w = gc.getWires(g);
      if (w.size() != 2) { ok = false; return 0.0; }
      double pa = contributorProb(gc, w[0], ref, ok);
      if (!ok) return 0.0;
      double pb = contributorProb(gc, w[1], ref, ok);
      if (!ok) return 0.0;
      return pa * (1.0 - pb);
    }
    default:
      ok = false;
      return 0.0;
  }
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
