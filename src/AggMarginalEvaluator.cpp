/**
 * @file AggMarginalEvaluator.cpp
 * @brief Implementation of the safe-join aggregate marginal-vector pre-pass
 *        (COUNT / SUM / MIN / MAX).  See @c AggMarginalEvaluator.h for the
 *        full docstring and the soundness argument.
 */
#include "AggMarginalEvaluator.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
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
 * conjunction of @c gate_input leaves: a bare @c gate_input, a
 * @c gate_one (deterministically-present, empty leaf set), or a
 * @c gate_times -- recursively, so a *nested* product
 * @c times(times(r,s),t) (e.g. an SPJ subquery / view whose tuple
 * provenance feeds an outer join) flattens to the same leaf set
 * @c {r,s,t} as the flat @c times(r,s,t).  This is sound on the
 * probability path: @c times is logical AND there, so it is
 * associative and the nesting does not change the conjunction's
 * probability (the non-commutativity of @c times matters only to the
 * symbolic semirings, which this pass never touches).  Any other shape
 * (gate_plus from a UNION, gate_monus, gate_mulinput) makes the cmp
 * bail.  A leaf repeated within the contributor also bails (the
 * read-once-within check below), since p^2 != p.
 *
 * On success @p out holds the contributor's leaf set (sorted, unique).
 */
static bool collectProductLeaves(GenericCircuit &gc, gate_t k,
                                 std::vector<gate_t> &out)
{
  switch (gc.getGateType(k)) {
    case gate_one:
      return true;                       /* identity factor: contributes nothing */
    case gate_input:
      out.push_back(k);
      return true;
    case gate_times:
      for (gate_t c : gc.getWires(k))
        if (!collectProductLeaves(gc, c, out)) return false;
      return true;
    default:
      return false;                      /* non-product factor: out of scope */
  }
}

static bool parseProductContributor(GenericCircuit &gc, gate_t k,
                                    std::vector<gate_t> &out)
{
  out.clear();
  if (!collectProductLeaves(gc, k, out))
    return false;
  /* Read-once within the contributor: a leaf used twice would make the
   * product probability wrong (p^2 vs the leaf's single mass). */
  std::sort(out.begin(), out.end());
  if (std::adjacent_find(out.begin(), out.end()) != out.end())
    return false;
  return true;
}

/* ------------------------------------------------------------------ *
 * Privacy of the aggregate subtree
 * ------------------------------------------------------------------ *
 * The cmp may be resolved to an independent Bernoulli only if all the
 * randomness it depends on is private to its own subtree -- i.e. no
 * gate reachable from the @c gate_agg is also referenced from elsewhere
 * in the circuit (which would couple the cmp's outcome to that other
 * use).  Walk the subtree rooted at @p agg and require, for every
 * non-constant gate in it, that its whole-circuit reference count
 * equals the number of references it receives from *within* the
 * subtree.  This subsumes (and generalises to nested / shared product
 * gates) the per-leaf @c ref==cnt and per-semimod @c ref==1 checks: a
 * subquery tuple's @c times(r,s) shared across several contributors is
 * internal (its internal ref count matches its total), so it passes;
 * any escape to an outside parent fails.  Constants (@c gate_one /
 * @c gate_zero / @c gate_value) carry no randomness and may be shared
 * freely, so they are exempt.  The caller separately requires
 * @c ref[agg]==1 (the agg is consumed by this cmp alone). */
static bool aggSubtreePrivate(GenericCircuit &gc, gate_t agg,
                              const std::vector<unsigned> &ref)
{
  std::map<gate_t, unsigned> internalRef;
  std::set<gate_t> visited;
  std::vector<gate_t> stk{agg};
  visited.insert(agg);
  while (!stk.empty()) {
    gate_t g = stk.back(); stk.pop_back();
    for (gate_t c : gc.getWires(g)) {
      ++internalRef[c];
      if (visited.insert(c).second) stk.push_back(c);
    }
  }
  for (gate_t g : visited) {
    if (g == agg) continue;
    switch (gc.getGateType(g)) {
      case gate_one: case gate_zero: case gate_value:
        continue;                        /* constants: sharing is harmless */
      default:
        break;
    }
    if (ref[static_cast<std::size_t>(g)] != internalRef[g])
      return false;                      /* referenced from outside the subtree */
  }
  return true;
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

/* Partition contributor indices into independence blocks: two
 * contributors are in the same block iff they (transitively) share a
 * leaf.  Independent blocks are combined by the aggregate's monoid; the
 * sharing inside a block is resolved by recursion. */
static std::vector<std::vector<int>> independenceBlocks(
    const std::vector<std::vector<gate_t>> &contribs)
{
  const std::size_t n = contribs.size();
  UnionFind uf(static_cast<int>(n));
  std::map<gate_t, int> first_owner;
  for (std::size_t i = 0; i < n; ++i)
    for (gate_t l : contribs[i]) {
      auto it = first_owner.find(l);
      if (it == first_owner.end()) first_owner[l] = static_cast<int>(i);
      else uf.unite(static_cast<int>(i), it->second);
    }
  std::map<int, std::vector<int>> bmap;
  for (std::size_t i = 0; i < n; ++i)
    bmap[uf.find(static_cast<int>(i))].push_back(static_cast<int>(i));
  std::vector<std::vector<int>> blocks;
  blocks.reserve(bmap.size());
  for (auto &kv : bmap) blocks.push_back(std::move(kv.second));
  return blocks;
}

/* Leaves common to *every* member of a block (this hierarchy level's
 * shared root event); empty when the members have no leaf in common,
 * which marks a non-laminar (non-hierarchical) structure. */
static std::vector<gate_t> commonLeaves(
    const std::vector<std::vector<gate_t>> &contribs,
    const std::vector<int> &members)
{
  std::vector<gate_t> common = contribs[members[0]];
  for (std::size_t mi = 1; mi < members.size() && !common.empty(); ++mi) {
    std::vector<gate_t> tmp;
    std::set_intersection(common.begin(), common.end(),
                          contribs[members[mi]].begin(),
                          contribs[members[mi]].end(),
                          std::back_inserter(tmp));
    common.swap(tmp);
  }
  return common;
}

/* Per-member residual leaf sets after removing this level's common root
 * leaves -- the structure one hierarchy level deeper. */
static std::vector<std::vector<gate_t>> residualsOf(
    const std::vector<std::vector<gate_t>> &contribs,
    const std::vector<int> &members, const std::vector<gate_t> &common)
{
  std::vector<std::vector<gate_t>> residuals;
  residuals.reserve(members.size());
  for (int m : members) {
    std::vector<gate_t> r;
    std::set_difference(contribs[m].begin(), contribs[m].end(),
                        common.begin(), common.end(), std::back_inserter(r));
    residuals.push_back(std::move(r));
  }
  return residuals;
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

/* Distribution of the product of two independent non-negative integer
 * counts: r[a*b] += A[a]*B[b].  Combines the per-factor count PMFs of a
 * Cartesian-product block (count = N_1 · N_2 · ...). */
static std::vector<double> productConvolve(const std::vector<double> &a,
                                           const std::vector<double> &b)
{
  if (a.empty() || b.empty()) return {};
  const std::size_t amax = a.size() - 1, bmax = b.size() - 1;
  std::vector<double> r(amax * bmax + 1, 0.0);
  for (std::size_t i = 0; i <= amax; ++i) {
    if (a[i] == 0.0) continue;
    for (std::size_t j = 0; j <= bmax; ++j)
      r[i * j] += a[i] * b[j];
  }
  return r;
}

/* Result of a Cartesian-product decomposition (see @c decomposeProduct). */
struct ProductDecomp {
  bool ok = false;                                  /* a complete leaf-disjoint product? */
  std::map<gate_t, int> leafFactor;                 /* leaf -> factor index */
  std::vector<std::vector<std::vector<gate_t>>> parts;  /* per factor: distinct parts */
};

/* Try to decompose a connected, common-less block into independent
 * Cartesian-product factors.  Two leaves share a factor iff they NEVER
 * co-occur in a contributor (united below); the factors are those
 * classes.  On success (@c ok) the block's contributors are exactly the
 * complete Cartesian product of the per-factor distinct parts, so the
 * block count is the product of the per-factor counts.  @c ok is false
 * when the block is not a complete leaf-disjoint product.
 *
 * This is what separates the safe cross-product (R(a),S(a,b),T(a,c) →
 * count = N_S·N_T) from the #P-hard h0 / triangle: h0 carries a private
 * "middle" leaf (the S(x,y) tuple, in exactly one contributor) that makes
 * leaves of different branches never co-occur, collapsing the factor
 * partition to one class and/or breaking |contributors| = ∏|parts|.  The
 * cross-product has no middle relation, so its branch leaves always
 * co-occur (completeness) and stay in separate factors.
 *
 * Soundness is a circuit-level fact independent of the query: a complete
 * leaf-disjoint product means each contributor is one part per factor,
 * present iff all its parts are; parts of distinct factors are
 * leaf-disjoint hence independent, so count = ∏ N_i exactly. */
static ProductDecomp decomposeProduct(
    const std::vector<std::vector<gate_t>> &contribs,
    const std::vector<int> &members)
{
  ProductDecomp out;

  /* Index the block's leaves. */
  std::vector<gate_t> L;
  for (int m : members) for (gate_t l : contribs[m]) L.push_back(l);
  std::sort(L.begin(), L.end());
  L.erase(std::unique(L.begin(), L.end()), L.end());
  std::map<gate_t, int> idx;
  for (std::size_t i = 0; i < L.size(); ++i) idx[L[i]] = static_cast<int>(i);
  const int nl = static_cast<int>(L.size());

  /* Co-occurrence: cooc[u][v] iff some member contains both leaves. */
  std::vector<std::vector<char>> cooc(nl, std::vector<char>(nl, 0));
  for (int m : members) {
    const auto &cl = contribs[m];
    for (std::size_t i = 0; i < cl.size(); ++i)
      for (std::size_t j = i + 1; j < cl.size(); ++j) {
        int a = idx[cl[i]], b = idx[cl[j]];
        cooc[a][b] = cooc[b][a] = 1;
      }
  }

  /* Factors = connected components under "never co-occur". */
  UnionFind uf(nl);
  for (int u = 0; u < nl; ++u)
    for (int v = u + 1; v < nl; ++v)
      if (!cooc[u][v]) uf.unite(u, v);
  std::map<int, int> factorId;
  for (int u = 0; u < nl; ++u)
    factorId.emplace(uf.find(u), static_cast<int>(factorId.size()));
  const int nf = static_cast<int>(factorId.size());
  if (nf < 2) return out;               /* single class: not a product */

  for (gate_t l : L) out.leafFactor[l] = factorId[uf.find(idx[l])];

  /* Project each member onto each factor; collect distinct parts.  A
   * member missing a factor is not a clean product. */
  std::vector<std::set<std::vector<gate_t>>> parts(nf);
  for (int m : members) {
    std::vector<std::vector<gate_t>> proj(nf);
    for (gate_t l : contribs[m])               /* member leaves are sorted */
      proj[out.leafFactor[l]].push_back(l);
    for (int f = 0; f < nf; ++f) {
      if (proj[f].empty()) return out;
      parts[f].insert(std::move(proj[f]));
    }
  }

  /* Completeness: |contributors| == product of per-factor part counts.
   * With the projection map injective (member = union of its parts), this
   * forces a bijection onto the full Cartesian product. */
  std::size_t prod = 1;
  for (int f = 0; f < nf; ++f) prod *= parts[f].size();
  if (prod != members.size()) return out;

  out.parts.resize(nf);
  for (int f = 0; f < nf; ++f)
    out.parts[f].assign(parts[f].begin(), parts[f].end());
  out.ok = true;
  return out;
}

/* Recursive count distribution over a set of product-of-leaves
 * contributors coupled only through a laminar (hierarchical) leaf-sharing
 * structure.  Returns the PMF @c m[c] = Pr(exactly c contributors present),
 * or clears @p ok (returning {}) when the sharing is non-laminar -- a
 * multi-member independence block with no leaf common to every member
 * (e.g. the triangle) -- which is outside the exact safe-plan class.
 *
 * This is the marginal-vector safe-plan engine, handling arbitrary
 * hierarchical depth:
 *  - partition the contributors into independent blocks by shared leaf
 *    (union-find); independent blocks combine by convolution (the ⊛^+
 *    combinator);
 *  - a singleton block is a Bernoulli over the product of its leaves;
 *  - a multi-member block with a leaf common to EVERY member factors out
 *    that shared root event: the block count is the disjoint mixture
 *    (1-p_root)·δ_0 + p_root·inner  (the ⊥ combinator), with @c inner the
 *    recursion on the per-member residual leaf sets (one level deeper);
 *  - a multi-member block with no common leaf is either a Cartesian
 *    product of independent factors (the join node: count = ∏ N_i,
 *    @c tryProductFactors + @c productConvolve) or a genuinely non-laminar
 *    tangle (h0 / triangle), which clears @p ok and bails.
 * Each recursion strips at least the common leaves, so the total leaf
 * count strictly decreases and the recursion terminates.  Depth-1 fan-out
 * is the case where every residual is a single leaf (inner becomes the
 * Poisson-binomial); deeper nesting (e.g. orders→items under a user)
 * recurses further. */
static std::vector<double> countPMF(GenericCircuit &gc,
                                    std::vector<std::vector<gate_t>> contribs,
                                    bool &ok)
{
  const std::size_t n = contribs.size();
  if (n == 0) return std::vector<double>{1.0};     /* δ_0 */

  /* Independence blocks: contributors sharing any leaf are coupled. */
  UnionFind uf(static_cast<int>(n));
  {
    std::map<gate_t, int> first_owner;
    for (std::size_t i = 0; i < n; ++i)
      for (gate_t l : contribs[i]) {
        auto it = first_owner.find(l);
        if (it == first_owner.end()) first_owner[l] = static_cast<int>(i);
        else uf.unite(static_cast<int>(i), it->second);
      }
  }
  std::map<int, std::vector<int>> blocks;
  for (std::size_t i = 0; i < n; ++i)
    blocks[uf.find(static_cast<int>(i))].push_back(static_cast<int>(i));

  std::vector<double> total{1.0};                  /* δ_0, convolution identity */
  for (const auto &be : blocks) {
    const std::vector<int> &members = be.second;
    std::vector<double> blockPMF;

    if (members.size() == 1) {
      /* One contributor: present iff all its leaves are -- a Bernoulli
       * over the product of the (independent) leaf marginals. */
      double q = 1.0;
      for (gate_t l : contribs[members[0]]) q *= gc.getProb(l);
      blockPMF = std::vector<double>{1.0 - q, q};
    } else {
      std::vector<gate_t> common = commonLeaves(contribs, members);
      if (!common.empty()) {
        /* Laminar: factor this level's shared root (the ⊥ mixture) and
         * recurse on the per-member residuals one level deeper. */
        double p_root = 1.0;
        for (gate_t l : common) p_root *= gc.getProb(l);
        std::vector<double> inner =
          countPMF(gc, residualsOf(contribs, members, common), ok);
        if (!ok) return {};
        blockPMF = std::move(inner);
        for (double &x : blockPMF) x *= p_root;        /* root-present arm */
        blockPMF[0] += (1.0 - p_root);                 /* root-absent: count 0 */
      } else {
        /* No shared root: the block is either a Cartesian product of
         * independent factors (the join node, count = ∏ N_i) or a
         * genuinely non-laminar tangle (h0 / triangle).
         * decomposeProduct distinguishes them on the circuit. */
        ProductDecomp pd = decomposeProduct(contribs, members);
        if (!pd.ok) { ok = false; return {}; }            /* non-hierarchical */
        std::vector<double> acc;
        for (std::size_t f = 0; f < pd.parts.size(); ++f) {
          std::vector<double> fp = countPMF(gc, std::move(pd.parts[f]), ok);
          if (!ok) return {};
          acc = (f == 0) ? std::move(fp) : productConvolve(acc, fp);
        }
        blockPMF = std::move(acc);
      }
    }
    total = convolve(total, blockPMF);
  }
  return total;
}

/* Tail-sum over the final count PMF under SQL HAVING semantics: sum the
 * mass of every count @c c with @c c >= 1 (empty group excluded) and
 * @c c op C true.  Mirrors CountCmpEvaluator::cdfForOperator exactly,
 * but driven by the materialised PMF rather than a Poisson-binomial. */
static double prFromPMF(const std::vector<double> &pmf,
                        ComparisonOperator op, long C)
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

/* ------------------------------------------------------------------ *
 * MIN / MAX
 * ------------------------------------------------------------------ *
 * P(every contributor's lineage is false) over a hierarchical set --
 * the scalar version of countPMF[0].  Independent blocks multiply; a
 * singleton block contributes (1 - product of its leaves); a
 * multi-member block is absent iff its shared root is absent, or the
 * root is present and all residuals are absent.  Clears @p ok on a
 * non-laminar block (no common leaf).  Every MIN/MAX HAVING predicate
 * reduces to a few calls of this on value-thresholded subsets, which is
 * the hierarchical generalisation of MinMaxCmpEvaluator's @c qprod
 * (product of @c 1-p_i over the matching independent children). */
static double pAllAbsent(GenericCircuit &gc,
                         std::vector<std::vector<gate_t>> contribs, bool &ok)
{
  if (contribs.empty()) return 1.0;
  double result = 1.0;
  for (const auto &members : independenceBlocks(contribs)) {
    double block_absent;
    if (members.size() == 1) {
      double q = 1.0;
      for (gate_t l : contribs[members[0]]) q *= gc.getProb(l);
      block_absent = 1.0 - q;
    } else {
      std::vector<gate_t> common = commonLeaves(contribs, members);
      if (!common.empty()) {
        double p_root = 1.0;
        for (gate_t l : common) p_root *= gc.getProb(l);
        double inner = pAllAbsent(gc, residualsOf(contribs, members, common), ok);
        if (!ok) return 0.0;
        block_absent = (1.0 - p_root) + p_root * inner;
      } else {
        /* Cartesian product: all contributors absent iff some factor is
         * entirely absent.  P = 1 - ∏_f (1 - pAllAbsent(factor_f)).  (When
         * a value-thresholded subset from minMaxProb is a sub-product this
         * is exactly the right combine; a non-product subset fails
         * decomposeProduct and bails, which is the sound action for the
         * #P-hard bipartite case.) */
        ProductDecomp pd = decomposeProduct(contribs, members);
        if (!pd.ok) { ok = false; return 0.0; }          /* non-hierarchical */
        double prodPresent = 1.0;
        for (auto &fparts : pd.parts) {
          double fa = pAllAbsent(gc, fparts, ok);
          if (!ok) return 0.0;
          prodPresent *= (1.0 - fa);
        }
        block_absent = 1.0 - prodPresent;
      }
    }
    result *= block_absent;
  }
  return result;
}

/* P(MIN/MAX(value) op C) over a hierarchical contributor set, empty
 * group excluded (a group with no present contributor has no min/max).
 * Each operator is a small combination of pAllAbsent over the subset of
 * contributors whose value satisfies a threshold predicate -- exactly
 * the decomposition in MinMaxCmpEvaluator, but with pAllAbsent in place
 * of the independent-only qprod, so it is exact on safe joins too. */
static double minMaxProb(GenericCircuit &gc,
                         const std::vector<std::vector<gate_t>> &leaves,
                         const std::vector<long> &vals,
                         AggregationOperator agg, ComparisonOperator op,
                         long C, bool &ok)
{
  /* P(all contributors whose value satisfies @p pred are absent). */
  auto pAbsentWhere = [&](auto pred) -> double {
    std::vector<std::vector<gate_t>> sub;
    for (std::size_t i = 0; i < leaves.size(); ++i)
      if (pred(vals[i])) sub.push_back(leaves[i]);
    return pAllAbsent(gc, std::move(sub), ok);
  };

  const double allAbsent = pAbsentWhere([](int) { return true; });
  double pr = 0.0;

  if (agg == AggregationOperator::MAX) {
    switch (op) {
      case ComparisonOperator::GE: pr = 1.0 - pAbsentWhere([&](long v){return v >= C;}); break;
      case ComparisonOperator::GT: pr = 1.0 - pAbsentWhere([&](long v){return v >  C;}); break;
      case ComparisonOperator::LE: pr = pAbsentWhere([&](long v){return v >  C;}) - allAbsent; break;
      case ComparisonOperator::LT: pr = pAbsentWhere([&](long v){return v >= C;}) - allAbsent; break;
      case ComparisonOperator::EQ: pr = pAbsentWhere([&](long v){return v >  C;})
                                      - pAbsentWhere([&](long v){return v >= C;}); break;
      case ComparisonOperator::NE: pr = (1.0 - allAbsent)
                                      - (pAbsentWhere([&](long v){return v >  C;})
                                       - pAbsentWhere([&](long v){return v >= C;})); break;
    }
  } else {  /* MIN */
    switch (op) {
      case ComparisonOperator::LE: pr = 1.0 - pAbsentWhere([&](long v){return v <= C;}); break;
      case ComparisonOperator::LT: pr = 1.0 - pAbsentWhere([&](long v){return v <  C;}); break;
      case ComparisonOperator::GE: pr = pAbsentWhere([&](long v){return v <  C;}) - allAbsent; break;
      case ComparisonOperator::GT: pr = pAbsentWhere([&](long v){return v <= C;}) - allAbsent; break;
      case ComparisonOperator::EQ: pr = pAbsentWhere([&](long v){return v <  C;})
                                      - pAbsentWhere([&](long v){return v <= C;}); break;
      case ComparisonOperator::NE: pr = (1.0 - allAbsent)
                                      - (pAbsentWhere([&](long v){return v <  C;})
                                       - pAbsentWhere([&](long v){return v <= C;})); break;
    }
  }
  return pr;
}

/* ------------------------------------------------------------------ *
 * SUM
 * ------------------------------------------------------------------ *
 * Reachable-sum support cap (Remark 3 pseudo-polynomial caveat): bail
 * when the sparse sum distribution would exceed this many distinct
 * values. */
constexpr std::size_t kMaxSumSupport = 1u << 20;

/* Does integer sum @p s satisfy @p s op C ?  Mirrors SumCmpEvaluator. */
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

/* Distribution of SUM(value) over a hierarchical contributor set, as a
 * sparse map sum -> probability.  Same recursion as countPMF, but a
 * present contributor adds its weight @p weights[i] (not 1), so blocks
 * combine by additive convolution over the (possibly negative) integer
 * sum domain.  COUNT is the all-weights-1 instance; this carries the
 * weighted case.  Clears @p ok on a non-laminar block or when the
 * support exceeds @c kMaxSumSupport. */
static std::map<long, double> sumPMF(GenericCircuit &gc,
                                     std::vector<std::vector<gate_t>> contribs,
                                     std::vector<long> weights, bool &ok)
{
  std::map<long, double> total;
  total[0] = 1.0;                                  /* δ_0 */
  if (contribs.empty()) return total;

  for (const auto &members : independenceBlocks(contribs)) {
    std::map<long, double> blockPMF;
    if (members.size() == 1) {
      double q = 1.0;
      for (gate_t l : contribs[members[0]]) q *= gc.getProb(l);
      blockPMF[0]                += 1.0 - q;        /* absent: contributes 0 */
      blockPMF[weights[members[0]]] += q;           /* present: contributes w */
    } else {
      std::vector<gate_t> common = commonLeaves(contribs, members);
      if (!common.empty()) {
        double p_root = 1.0;
        for (gate_t l : common) p_root *= gc.getProb(l);

        std::vector<std::vector<gate_t>> residuals =
          residualsOf(contribs, members, common);
        std::vector<long> rweights;
        rweights.reserve(members.size());
        for (int m : members) rweights.push_back(weights[m]);

        std::map<long, double> inner =
          sumPMF(gc, std::move(residuals), std::move(rweights), ok);
        if (!ok) return {};
        for (const auto &kv : inner) blockPMF[kv.first] += p_root * kv.second;
        blockPMF[0] += (1.0 - p_root);              /* root absent: sum 0 */
      } else {
        /* Cartesian product.  SUM factors only when the value depends on a
         * single factor f: SUM = S_f · M, with S_f the weighted sum over
         * factor f and M = ∏_{i≠f} N_i the count-product of the others
         * (independent).  Detect such an f (weight constant within each
         * f-part group); otherwise the value couples factors and the case
         * may be #P-hard, so bail. */
        ProductDecomp pd = decomposeProduct(contribs, members);
        if (!pd.ok) { ok = false; return {}; }
        const int nf = static_cast<int>(pd.parts.size());

        int chosen = -1;
        std::map<std::vector<gate_t>, long> partVal;
        for (int f = 0; f < nf && chosen < 0; ++f) {
          std::map<std::vector<gate_t>, long> pv;
          bool consistent = true;
          for (int m : members) {
            std::vector<gate_t> partf;
            for (gate_t l : contribs[m])
              if (pd.leafFactor[l] == f) partf.push_back(l);
            auto it = pv.find(partf);
            if (it == pv.end()) pv[partf] = weights[m];
            else if (it->second != weights[m]) { consistent = false; break; }
          }
          if (consistent) { chosen = f; partVal = std::move(pv); }
        }
        if (chosen < 0) { ok = false; return {}; }   /* value spans factors */

        /* S_f: weighted-sum distribution over the chosen factor's parts. */
        std::vector<long> partValues;
        partValues.reserve(pd.parts[chosen].size());
        for (const auto &part : pd.parts[chosen])
          partValues.push_back(partVal[part]);
        std::map<long, double> Sf =
          sumPMF(gc, pd.parts[chosen], std::move(partValues), ok);
        if (!ok) return {};

        /* M: count-product distribution over the other factors. */
        std::vector<double> M;
        for (int f = 0; f < nf; ++f) {
          if (f == chosen) continue;
          std::vector<double> cf = countPMF(gc, pd.parts[f], ok);
          if (!ok) return {};
          M = M.empty() ? std::move(cf) : productConvolve(M, cf);
        }

        /* blockPMF = distribution of S_f · M (independent factors). */
        for (const auto &skv : Sf)
          for (std::size_t mm = 0; mm < M.size(); ++mm)
            if (M[mm] != 0.0)
              blockPMF[skv.first * static_cast<long>(mm)] += skv.second * M[mm];
        if (blockPMF.size() > kMaxSumSupport) { ok = false; return {}; }
      }
    }
    std::map<long, double> ntotal;
    for (const auto &a : total)
      for (const auto &b : blockPMF)
        ntotal[a.first + b.first] += a.second * b.second;
    if (ntotal.size() > kMaxSumSupport) { ok = false; return {}; }
    total.swap(ntotal);
  }
  return total;
}

}  // namespace

unsigned runAggMarginalEvaluator(GenericCircuit &gc)
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
    const AggregationOperator agg_kind = match.agg_kind;
    if (agg_kind != AggregationOperator::COUNT &&
        agg_kind != AggregationOperator::SUM   &&
        agg_kind != AggregationOperator::AVG   &&
        agg_kind != AggregationOperator::MIN   &&
        agg_kind != AggregationOperator::MAX)
      continue;                          /* other aggregates: out of scope */

    const gate_t agg = match.agg;
    const auto &ks = match.ks;
    const std::size_t n = ks.size();

    /* The aggregate must be consumed by this cmp alone: a shared agg
     * would couple two HAVING comparators over the same aggregate. */
    if (ref[static_cast<std::size_t>(agg)] != 1) continue;
    bool ok = true;

    /* Parse every contributor into its (flattened) leaf set. */
    std::vector<std::vector<gate_t>> leaves(n);
    for (std::size_t i = 0; i < n && ok; ++i)
      if (!parseProductContributor(gc, ks[i], leaves[i])) ok = false;
    if (!ok) continue;

    /* The cmp's randomness must be private to its agg subtree -- no gate
     * reachable from the agg referenced from outside it -- the soundness
     * precondition for resolving the cmp to an independent Bernoulli.
     * Subsumes the per-semimod ref==1 and per-leaf ref==cnt checks and
     * extends them to nested / shared product gates (subquery tuples). */
    if (!aggSubtreePrivate(gc, agg, ref)) continue;

    /* Dispatch on the aggregate; each arm computes the exact probability
     * over the hierarchical (laminar) contributor structure, recursing
     * through shared root events.  A non-laminar shape clears @c ok and
     * the cmp falls back to exact enumeration. */
    double pr;
    if (agg_kind == AggregationOperator::COUNT) {
      std::vector<double> total = countPMF(gc, leaves, ok);
      if (!ok) continue;
      pr = prFromPMF(total, match.op, match.C);
    } else if (agg_kind == AggregationOperator::SUM ||
               agg_kind == AggregationOperator::AVG) {
      /* SUM(v) θ C directly; AVG(v) θ C ⟺ SUM(v_i − C) θ 0 (multiply the
       * average by the positive group count; the empty group has no
       * average and is excluded, exactly as the empty group is for SUM).
       * Both reduce to the weighted-sum distribution, so AVG inherits the
       * laminar / product machinery for free.  Only integer thresholds
       * reach here -- a fractional HAVING-AVG constant is rejected upstream
       * before the cmp is even built. */
      const bool is_avg = (agg_kind == AggregationOperator::AVG);
      std::vector<long> weights;
      weights.reserve(match.ms.size());
      long lo = 0, hi = 0;
      for (int m : match.ms) {
        long w = is_avg ? (static_cast<long>(m) - match.C) : static_cast<long>(m);
        weights.push_back(w);
        if (w < 0) lo += w; else hi += w;
      }
      /* Reachable-sum range cap (Remark 3 pseudo-polynomial caveat). */
      if (hi - lo + 1 > static_cast<long>(kMaxSumSupport)) continue;
      const long thr = is_avg ? 0 : match.C;

      std::map<long, double> dist = sumPMF(gc, leaves, std::move(weights), ok);
      if (!ok) continue;
      pr = 0.0;
      for (const auto &kv : dist)
        if (sumSatisfies(kv.first, match.op, thr)) pr += kv.second;
      /* Exclude the empty group: its (shifted) sum is 0, so subtract its
       * mass when 0 satisfies the predicate (a non-empty group that
       * happens to sum to the threshold stays). */
      if (sumSatisfies(0, match.op, thr)) {
        double emptyMass = pAllAbsent(gc, leaves, ok);
        if (!ok) continue;
        pr -= emptyMass;
      }
    } else {  /* MIN or MAX */
      pr = minMaxProb(gc, leaves, match.ms, agg_kind, match.op, match.C, ok);
      if (!ok) continue;
    }

    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
