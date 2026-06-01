/**
 * @file AggMarginalEvaluator.cpp
 * @brief Implementation of the safe-join COUNT marginal-vector pre-pass.
 *        See @c AggMarginalEvaluator.h for the full docstring and the
 *        soundness argument.
 */
#include "AggMarginalEvaluator.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <vector>

#include "Aggregation.h"          // AggregationOperator + ComparisonOperator
#include "CmpEvaluatorCommon.h"   // matchAggCmp, computeRefCounts

extern "C" {
#include "provsql_utils.h"        // gate_type enum
}

namespace provsql {

namespace {

/* ------------------------------------------------------------------ *
 * Contributor parsing
 * ------------------------------------------------------------------ *
 * A contributor (the K side of a semimod) is in scope iff it is a
 * conjunction of distinct @c gate_input leaves: a bare @c gate_input,
 * a @c gate_one (deterministically-present, empty leaf set), or a
 * @c gate_times all of whose children are @c gate_input / @c gate_one.
 * Any other shape (gate_plus from a UNION, gate_monus, gate_mulinput,
 * nested products, a leaf repeated within the contributor) is out of
 * the first-slice scope and makes the whole cmp bail.
 *
 * On success @p out holds the contributor's leaf set (sorted, unique).
 */
static bool parseProductContributor(GenericCircuit &gc, gate_t k,
                                    std::vector<gate_t> &out)
{
  out.clear();
  switch (gc.getGateType(k)) {
    case gate_one:
      return true;                       /* empty conjunction: always present */
    case gate_input:
      out.push_back(k);
      return true;
    case gate_times: {
      for (gate_t c : gc.getWires(k)) {
        switch (gc.getGateType(c)) {
          case gate_one:
            break;                       /* identity factor, skip */
          case gate_input:
            out.push_back(c);
            break;
          default:
            return false;                /* non-leaf factor: out of scope */
        }
      }
      /* Read-once within the contributor: a leaf used twice would make
       * the product probability wrong (p^2 vs the leaf's single mass). */
      std::sort(out.begin(), out.end());
      if (std::adjacent_find(out.begin(), out.end()) != out.end())
        return false;
      return true;
    }
    default:
      return false;
  }
}

/* Disjoint-set forest over contributor indices, union by shared leaf. */
struct UnionFind {
  std::vector<int> parent;
  explicit UnionFind(int n) : parent(n) {
    std::iota(parent.begin(), parent.end(), 0);
  }
  int find(int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  }
  void unite(int a, int b) { parent[find(a)] = find(b); }
};

/* Poisson-binomial PMF over the marginals @p p : full vector
 * @c pmf[j] = Pr(exactly j of the |p| Bernoullis succeed), j in [0,|p|].
 * O(|p|^2); the block sizes here are small. */
static std::vector<double> poissonBinomialPMF(const std::vector<double> &p)
{
  std::vector<double> pmf(p.size() + 1, 0.0);
  pmf[0] = 1.0;
  std::size_t filled = 0;
  for (double pi : p) {
    const double qi = 1.0 - pi;
    /* Single-pass downward recurrence (mirrors
     * CountCmpEvaluator::partialPMF): iterate j high-to-low so each
     * read of pmf[j-1] sees the not-yet-updated previous row.  A
     * split add-then-scale would wrongly multiply the success arm
     * pmf[j-1]*pi by qi as well. */
    for (std::size_t j = filled + 1; j >= 1; --j)
      pmf[j] = pmf[j] * qi + pmf[j - 1] * pi;
    pmf[0] *= qi;
    ++filled;
  }
  return pmf;
}

/* Convolution of two count PMFs (independent sum of the two counts). */
static std::vector<double> convolve(const std::vector<double> &a,
                                    const std::vector<double> &b)
{
  if (a.empty()) return b;
  if (b.empty()) return a;
  std::vector<double> r(a.size() + b.size() - 1, 0.0);
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] == 0.0) continue;
    for (std::size_t j = 0; j < b.size(); ++j)
      r[i + j] += a[i] * b[j];
  }
  return r;
}

/* Tail-sum over the final count PMF under SQL HAVING semantics: sum the
 * mass of every count @c c with @c c >= 1 (empty group excluded) and
 * @c c op C true.  Mirrors CountCmpEvaluator::cdfForOperator exactly,
 * but driven by the materialised PMF rather than a Poisson-binomial. */
static double prFromPMF(const std::vector<double> &pmf,
                        ComparisonOperator op, int C)
{
  double pr = 0.0;
  for (int c = 1; c < static_cast<int>(pmf.size()); ++c) {
    bool sat = false;
    switch (op) {
      case ComparisonOperator::GE: sat = (c >= C); break;
      case ComparisonOperator::GT: sat = (c >  C); break;
      case ComparisonOperator::LE: sat = (c <= C); break;
      case ComparisonOperator::LT: sat = (c <  C); break;
      case ComparisonOperator::EQ: sat = (c == C); break;
      case ComparisonOperator::NE: sat = (c != C); break;
    }
    if (sat) pr += pmf[c];
  }
  return pr;
}

}  // namespace

unsigned runAggMarginalCountEvaluator(GenericCircuit &gc)
{
  unsigned resolved = 0;
  const auto nb = gc.getNbGates();

  std::vector<gate_t> cmps;
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) == gate_cmp)
      cmps.push_back(g);
  }
  if (cmps.empty()) return 0;

  auto ref = computeRefCounts(gc);

  for (gate_t cmp : cmps) {
    if (gc.getGateType(cmp) != gate_cmp) continue;  /* resolved meanwhile */

    AggCmpMatch match;
    if (!matchAggCmp(gc, cmp, match))
      continue;
    if (match.agg_kind != AggregationOperator::COUNT)
      continue;
    {
      bool all_one = true;
      for (int m : match.ms) if (m != 1) { all_one = false; break; }
      if (!all_one) continue;            /* weighted COUNT: SUM's job */
    }

    const gate_t agg = match.agg;
    const auto &semimods = match.semimods;
    const auto &ks = match.ks;
    const std::size_t n = ks.size();

    /* The aggregate and each wrapper must be consumed by this cmp / agg
     * alone, exactly as in CountCmpEvaluator: a shared agg would couple
     * two HAVING comparators over the same count. */
    if (ref[static_cast<std::size_t>(agg)] != 1) continue;
    bool ok = true;
    for (gate_t sm : semimods)
      if (ref[static_cast<std::size_t>(sm)] != 1) { ok = false; break; }
    if (!ok) continue;

    /* Parse every contributor into its leaf set; tally per-leaf how many
     * contributors reference it (the cmp-internal reference count). */
    std::vector<std::vector<gate_t>> leaves(n);
    std::map<gate_t, unsigned> cnt;
    for (std::size_t i = 0; i < n && ok; ++i) {
      if (!parseProductContributor(gc, ks[i], leaves[i])) { ok = false; break; }
      for (gate_t l : leaves[i]) ++cnt[l];
    }
    if (!ok) continue;

    /* No outside reachability: every involved leaf's whole-circuit ref
     * count must equal its cmp-internal count, so resolving the cmp to an
     * independent Bernoulli does not sever a correlation with the rest of
     * the circuit.  Generalises CountCmpEvaluator's ref==1 (which forbids
     * sharing entirely) to "shared only among these contributors". */
    for (const auto &kv : cnt)
      if (ref[static_cast<std::size_t>(kv.first)] != kv.second) { ok = false; break; }
    if (!ok) continue;

    /* Union contributors that share any leaf into independence blocks. */
    UnionFind uf(static_cast<int>(n));
    {
      std::map<gate_t, int> first_owner;
      for (std::size_t i = 0; i < n; ++i)
        for (gate_t l : leaves[i]) {
          auto it = first_owner.find(l);
          if (it == first_owner.end()) first_owner[l] = static_cast<int>(i);
          else uf.unite(static_cast<int>(i), it->second);
        }
    }
    std::map<int, std::vector<int>> blocks;
    for (std::size_t i = 0; i < n; ++i)
      blocks[uf.find(static_cast<int>(i))].push_back(static_cast<int>(i));

    /* Per block: factor out the leaves common to *every* member (the
     * shared root event), require the per-member private leaf sets to be
     * pairwise disjoint, and build the block's count PMF as the disjoint
     * mixture  (1 - p_root) delta_0 + p_root * PB(private marginals). */
    std::vector<double> total;          /* running convolution; empty = delta_0 implicit */
    for (const auto &be : blocks) {
      const std::vector<int> &members = be.second;

      /* common = intersection of all members' leaf sets (sorted vectors). */
      std::vector<gate_t> common = leaves[members[0]];
      for (std::size_t mi = 1; mi < members.size() && !common.empty(); ++mi) {
        std::vector<gate_t> tmp;
        std::set_intersection(common.begin(), common.end(),
                              leaves[members[mi]].begin(),
                              leaves[members[mi]].end(),
                              std::back_inserter(tmp));
        common.swap(tmp);
      }
      /* A multi-member block unioned with no leaf common to all members is
       * a chain / non-hierarchical shape (e.g. the triangle): out of
       * scope, bail the whole cmp to exact enumeration. */
      if (members.size() > 1 && common.empty()) { ok = false; break; }

      double p_root = 1.0;
      for (gate_t l : common) p_root *= gc.getProb(l);

      /* Private marginals, checking pairwise disjointness across members. */
      std::vector<double> priv_p;
      std::vector<gate_t> seen_private;
      priv_p.reserve(members.size());
      for (int m : members) {
        std::vector<gate_t> priv;
        std::set_difference(leaves[m].begin(), leaves[m].end(),
                            common.begin(), common.end(),
                            std::back_inserter(priv));
        for (gate_t l : priv) {
          if (std::binary_search(seen_private.begin(), seen_private.end(), l)) {
            ok = false;                  /* a private leaf shared by two members */
            break;
          }
        }
        if (!ok) break;
        for (gate_t l : priv) {
          seen_private.insert(
            std::lower_bound(seen_private.begin(), seen_private.end(), l), l);
        }
        double pp = 1.0;
        for (gate_t l : priv) pp *= gc.getProb(l);
        priv_p.push_back(pp);
      }
      if (!ok) break;

      std::vector<double> blockPMF = poissonBinomialPMF(priv_p);
      for (double &m : blockPMF) m *= p_root;     /* root-present arm */
      blockPMF[0] += (1.0 - p_root);              /* root-absent arm: count 0 */

      total = convolve(total, blockPMF);
    }
    if (!ok) continue;
    if (total.empty()) continue;        /* no contributors: leave to enumeration */

    double pr = prFromPMF(total, match.op, match.C);
    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
