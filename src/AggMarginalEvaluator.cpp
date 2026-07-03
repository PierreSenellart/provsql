/**
 * @file AggMarginalEvaluator.cpp
 * @brief Implementation of the safe-join aggregate marginal-vector pre-pass
 *        (COUNT / SUM / MIN / MAX).  See @c AggMarginalEvaluator.h for the
 *        full docstring and the soundness argument.
 */
#include "AggMarginalEvaluator.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <type_traits>
#include <vector>

#include "Aggregation.h"          // AggregationOperator + ComparisonOperator
#include "CmpEvaluatorCommon.h"   // matchAggCmp, computeRefCounts
#include "RandomVariable.h"       // parseDoubleStrict

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

/* Brute-force leaf cap for the exact private-contributor marginal. */
constexpr unsigned kMaxContributorLeaves = 20;

/* Exact marginal probability of a contributor (a semimod K side) that is a
 * *private* Boolean sub-circuit over @c input leaves -- @c plus / @c times /
 * @c monus and the @c one / @c zero constants -- even when it is *not*
 * read-once internally.  This is the UNION / EXCEPT-over-a-shared-base-tuple
 * shape: a contributor @c (r∧s)∨(r∧t) or @c (r∧s)∖(r∧t) repeats the joined
 * leaf @c r, which @c contributorProb (read-once only) rejects.
 *
 * "Private" means every gate in the cone below the root is referenced only
 * from within the cone (whole-circuit @c ref == the cone-internal reference
 * count).  That single condition gives independence from every *other*
 * contributor (their footprints are disjoint -- a shared leaf would have an
 * external reference), so the contributor is an independent event whose exact
 * probability the caller can treat as a one-alternative BID block.  Computed
 * by brute force over the cone's distinct inputs (the internal sharing is
 * resolved exactly; capped at @c kMaxContributorLeaves).  Returns false --
 * caller bails to enumeration -- when the cone is not private (shared with
 * another contributor: the genuinely #P-hard case), too large, or holds an
 * unsupported gate. */
static bool contributorExactMarginal(GenericCircuit &gc, gate_t g,
                                     const std::vector<unsigned> &ref,
                                     double &out)
{
  /* Iterative post-order over the cone; count cone-internal references. */
  std::map<gate_t, unsigned> internalRef;
  std::set<gate_t> seen;
  std::vector<gate_t> order;                   /* children before parents */
  std::vector<std::pair<gate_t, bool>> stk{{g, false}};
  while (!stk.empty()) {
    auto top = stk.back(); stk.pop_back();
    gate_t x = top.first;
    const auto t = gc.getGateType(x);
    if (t != gate_one && t != gate_zero && t != gate_input &&
        t != gate_times && t != gate_plus && t != gate_monus)
      return false;                            /* unsupported gate in cone */
    if (top.second) { order.push_back(x); continue; }
    if (!seen.insert(x).second) continue;
    stk.push_back({x, true});
    if (t == gate_times || t == gate_plus || t == gate_monus)
      for (gate_t c : gc.getWires(x)) {
        ++internalRef[c];
        stk.push_back({c, false});
      }
  }

  /* Privacy: every non-constant cone gate but the root used only inside. */
  for (gate_t x : seen) {
    if (x == g) continue;                      /* root: ref checked by caller */
    switch (gc.getGateType(x)) {
      case gate_one: case gate_zero: continue;
      default: break;
    }
    if (ref[static_cast<std::size_t>(x)] != internalRef[x]) return false;
  }

  /* Compact, pre-resolved representation for the inner loop. */
  const int N = static_cast<int>(order.size());
  std::map<gate_t, int> pos;
  for (int i = 0; i < N; ++i) pos[order[i]] = i;
  std::vector<gate_type> typ(N);
  std::vector<std::vector<int>> childIdx(N);
  std::vector<int> leafbit(N, -1);
  std::vector<double> leafProb;
  for (int i = 0; i < N; ++i) {
    typ[i] = gc.getGateType(order[i]);
    if (typ[i] == gate_input) {
      leafbit[i] = static_cast<int>(leafProb.size());
      leafProb.push_back(gc.getProb(order[i]));
    } else {
      for (gate_t c : gc.getWires(order[i])) childIdx[i].push_back(pos[c]);
    }
  }
  const unsigned m = static_cast<unsigned>(leafProb.size());
  if (m > kMaxContributorLeaves) return false;

  /* Σ over assignments where the root is true of ∏ leaf marginals. */
  std::vector<char> val(N);
  double total = 0.0;
  for (uint32_t mask = 0; mask < (1u << m); ++mask) {
    for (int i = 0; i < N; ++i) {
      switch (typ[i]) {
        case gate_one:   val[i] = 1; break;
        case gate_zero:  val[i] = 0; break;
        case gate_input: val[i] = (mask >> leafbit[i]) & 1u; break;
        case gate_times: { char v = 1; for (int c : childIdx[i]) v = v && val[c]; val[i] = v; break; }
        case gate_plus:  { char v = 0; for (int c : childIdx[i]) v = v || val[c]; val[i] = v; break; }
        case gate_monus: val[i] = val[childIdx[i][0]] && !val[childIdx[i][1]]; break;
        default: return false;
      }
    }
    if (val[N - 1]) {                          /* root is last in post-order */
      double pr = 1.0;
      for (unsigned b = 0; b < m; ++b)
        pr *= (mask >> b) & 1u ? leafProb[b] : 1.0 - leafProb[b];
      total += pr;
    }
  }
  out = total;
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
 * but driven by the materialised PMF rather than a Poisson-binomial.
 *
 * For a scalar aggregation (@p is_scalar) the empty input is a real world
 * (one row, count 0), so the sum starts at @c c = 0 and @c pmf[0] is
 * included when @c 0 op C holds. */
static double prFromPMF(const std::vector<double> &pmf,
                        ComparisonOperator op, long C, bool is_scalar)
{
  double pr = 0.0;
  for (int c = is_scalar ? 0 : 1; c < static_cast<int>(pmf.size()); ++c) {
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
                         const std::vector<std::vector<std::pair<long, double>>> &blocks,
                         AggregationOperator agg, ComparisonOperator op,
                         long C, bool &ok)
{
  /* P(all contributors whose value satisfies @p pred are absent).  The TID
   * part goes through the hierarchical pAllAbsent; each independent BID block
   * contributes (1 - Σ_{alt: pred} p_alt) -- the probability its (single)
   * present alternative is not one whose value satisfies @p pred (mutual
   * exclusion: the matching subset is all-absent iff the chosen one, if any,
   * lies outside it). */
  auto pAbsentWhere = [&](auto pred) -> double {
    std::vector<std::vector<gate_t>> sub;
    for (std::size_t i = 0; i < leaves.size(); ++i)
      if (pred(vals[i])) sub.push_back(leaves[i]);
    double r = pAllAbsent(gc, std::move(sub), ok);
    for (const auto &blk : blocks) {
      double s = 0.0;
      for (const auto &alt : blk) if (pred(alt.first)) s += alt.second;
      r *= 1.0 - (s > 1.0 ? 1.0 : s);
    }
    return r;
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

/* Joint (sum, count) distribution over a contributor set, as a sparse map
 * (sum, count) -> probability.  Generalises @c countPMF / @c sumPMF to
 * track both coordinates at once.  This is what the *branch-spanning* SUM
 * needs: when an additively-separable value spans several product factors,
 * the block sum is Σ_f sum_f · ∏_{g≠f} cnt_g, which couples each factor's
 * weighted sum to the others' counts -- so neither marginal alone carries
 * enough information and the per-factor *joint* must be folded.  Same
 * laminar recursion as @c sumPMF; clears @p ok on a non-laminar block, a
 * non-separable product value, or a support overflow.
 *
 * Templated on the weight (sum-coordinate) type: the HAVING cmp path
 * instantiates @c long (its constants and grid arithmetic are integer);
 * the AVG moment instantiates @c double (arbitrary numeric row values).
 * The singleton and laminar-shared-root branches are weight-agnostic;
 * only the additive-separation recovery over a Cartesian-product block
 * is genuinely integer arithmetic and is compiled for the integral
 * instantiation alone (the double instantiation self-gates to the
 * caller's fallback there). */
template <typename W>
using JointPMFT = std::map<std::pair<W, long>, double>;
using JointPMF = JointPMFT<long>;

/* Recover an additive separation of a product block's weights across its
 * factors: find per-factor part values @p partVals (aligned to
 * @c pd.parts[f]) with weights[m] == Σ_f partVals[f][part_f(m)], the
 * constant folded into factor 0.  Uses the reference-axis construction on
 * the complete grid (h_f(p) = w(ref but p at f) - w(ref)); verifies the
 * separation reproduces every member.  Returns false -- caller bails --
 * when the value is not additively separable (it genuinely couples factors
 * and may be #P-hard, e.g. a product of two branches). */
static bool recoverAdditiveSeparation(
    const std::vector<std::vector<gate_t>> &contribs,
    const std::vector<int> &members, const std::vector<long> &weights,
    const ProductDecomp &pd, std::vector<std::vector<long>> &partVals)
{
  const int nf = static_cast<int>(pd.parts.size());

  auto partOf = [&](int m, int f) {
    std::vector<gate_t> p;
    for (gate_t l : contribs[m])                  /* contribs[m] already sorted */
      if (pd.leafFactor.at(l) == f) p.push_back(l);
    return p;
  };

  /* Complete-grid lookup: full part-tuple -> weight. */
  std::map<std::vector<std::vector<gate_t>>, long> grid;
  for (int m : members) {
    std::vector<std::vector<gate_t>> key(nf);
    for (int f = 0; f < nf; ++f) key[f] = partOf(m, f);
    grid[key] = weights[m];
  }

  const int m0 = members[0];
  const long W0 = weights[m0];
  std::vector<std::vector<gate_t>> ref(nf);
  for (int f = 0; f < nf; ++f) ref[f] = partOf(m0, f);

  /* h_f(p) = w(ref, but part p at factor f) - W0  (so h_f(ref_f) = 0). */
  std::vector<std::map<std::vector<gate_t>, long>> h(nf);
  for (int f = 0; f < nf; ++f)
    for (const auto &p : pd.parts[f]) {
      std::vector<std::vector<gate_t>> key = ref;
      key[f] = p;
      auto it = grid.find(key);
      if (it == grid.end()) return false;         /* incomplete grid */
      h[f][p] = it->second - W0;
    }

  /* The separation must reproduce every member's weight. */
  for (int m : members) {
    long acc = W0;
    for (int f = 0; f < nf; ++f) acc += h[f].at(partOf(m, f));
    if (acc != weights[m]) return false;          /* not additively separable */
  }

  partVals.assign(nf, {});
  for (int f = 0; f < nf; ++f) {
    partVals[f].reserve(pd.parts[f].size());
    for (const auto &p : pd.parts[f])
      partVals[f].push_back(h[f].at(p) + (f == 0 ? W0 : 0));  /* fold W0 into f=0 */
  }
  return true;
}

template <typename W>
static JointPMFT<W> sumCountPMF(GenericCircuit &gc,
                                std::vector<std::vector<gate_t>> contribs,
                                std::vector<W> weights, bool &ok)
{
  JointPMFT<W> total;
  total[{0, 0}] = 1.0;                             /* δ_(0,0) */
  if (contribs.empty()) return total;

  for (const auto &members : independenceBlocks(contribs)) {
    JointPMFT<W> blockPMF;
    if (members.size() == 1) {
      double q = 1.0;
      for (gate_t l : contribs[members[0]]) q *= gc.getProb(l);
      blockPMF[{0, 0}]                  += 1.0 - q;     /* absent: (0,0) */
      blockPMF[{weights[members[0]], 1}] += q;          /* present: (w,1) */
    } else {
      std::vector<gate_t> common = commonLeaves(contribs, members);
      if (!common.empty()) {
        /* Laminar shared root: disjoint mixture, recurse on residuals. */
        double p_root = 1.0;
        for (gate_t l : common) p_root *= gc.getProb(l);
        std::vector<W> rweights;
        rweights.reserve(members.size());
        for (int m : members) rweights.push_back(weights[m]);
        JointPMFT<W> inner = sumCountPMF(
          gc, residualsOf(contribs, members, common), std::move(rweights), ok);
        if (!ok) return {};
        for (const auto &kv : inner) blockPMF[kv.first] += p_root * kv.second;
        blockPMF[{0, 0}] += 1.0 - p_root;          /* root absent: (0,0) */
      } else if constexpr (std::is_integral_v<W>) {
        /* Cartesian product of independent factors.  An additively
         * separable value folds per-factor joints with the product
         * combinator (S,N) ⊗ (s,n) = (S·n + s·N, N·n), identity (0,1):
         * count multiplies, sum picks up each factor's weighted sum times
         * the others' counts.  This is exactly Σ_f sum_f · ∏_{g≠f} cnt_g.
         * The separation recovery is exact integer grid arithmetic, hence
         * integral instantiations only. */
        ProductDecomp pd = decomposeProduct(contribs, members);
        if (!pd.ok) { ok = false; return {}; }
        std::vector<std::vector<long>> partVals;
        if (!recoverAdditiveSeparation(contribs, members, weights, pd,
                                       partVals)) {
          ok = false; return {};                   /* value couples factors */
        }
        JointPMFT<W> acc;
        acc[{0, 1}] = 1.0;                          /* empty product: (0,1) */
        for (std::size_t f = 0; f < pd.parts.size(); ++f) {
          JointPMFT<W> Jf = sumCountPMF(gc, pd.parts[f], partVals[f], ok);
          if (!ok) return {};
          JointPMFT<W> nacc;
          for (const auto &a : acc)
            for (const auto &b : Jf)
              nacc[{a.first.first * b.first.second
                      + b.first.first * a.first.second,
                    a.first.second * b.first.second}] += a.second * b.second;
          if (nacc.size() > kMaxSumSupport) { ok = false; return {}; }
          acc.swap(nacc);
        }
        blockPMF = std::move(acc);
      } else {
        /* Non-laminar product block under a non-integral weight type:
         * out of the double instantiation's scope. */
        ok = false; return {};
      }
    }
    /* Independent blocks: sums and counts add. */
    JointPMFT<W> ntotal;
    for (const auto &a : total)
      for (const auto &b : blockPMF)
        ntotal[{a.first.first + b.first.first,
                a.first.second + b.first.second}] += a.second * b.second;
    if (ntotal.size() > kMaxSumSupport) { ok = false; return {}; }
    total.swap(ntotal);
  }
  return total;
}

static std::map<long, double> sumPMF(GenericCircuit &gc,
                                     std::vector<std::vector<gate_t>> contribs,
                                     std::vector<long> weights, bool &ok);

/* Overflow-checked 128-bit multiply with magnitude headroom: @c false on
 * wraparound or a result past @c LIM (so the caller bails to enumeration,
 * still correct).  Used by the multiplicative-separable fold, whose
 * intermediate products of per-factor sums can be large. */
static bool i128_mul(__int128 a, __int128 b, __int128 &out)
{
  constexpr __int128 LIM = static_cast<__int128>(1) << 120;
  if (a == 0 || b == 0) { out = 0; return true; }
  __int128 r = a * b;
  if (r / b != a) return false;                   /* wrapped */
  __int128 ar = r < 0 ? -r : r;
  if (ar > LIM) return false;                      /* keep headroom for later ops */
  out = r;
  return true;
}

/* SUM distribution of a Cartesian-product block whose value is
 * *multiplicatively* separable across the factors, w_m = ∏_f v_f(part_f):
 * then SUM = ∏_f sum_f with sum_f = Σ_{present p} v_f(p), so the block sum
 * is a product of independent per-factor weighted sums.  No explicit
 * factorisation is needed -- with a nonzero pivot weight @c D at reference
 * parts and the grid's axis entries A^f_p = w(ref but p at f), the identity
 *   block sum = ∏_f (Σ_{present p} A^f_p) / D^{nf-1}
 * holds (each axis sum carries a spurious factor D/v_f(ref_f), and the nf
 * of them divide back to D^{nf-1}).  The A^f_p are grid entries (integers),
 * so per-factor @c sumPMF gives Σ A^f_p exactly; the per-factor sum PMFs are
 * product-convolved (in 128-bit, guarded) and divided by D^{nf-1} (exact in
 * every world, since each world's block sum is integral).  Clears @p ok --
 * caller bails to enumeration -- when the value is not multiplicatively
 * separable, on overflow, or on a within-factor non-laminar bail. */
static std::map<long, double> mulSeparableSumPMF(
    GenericCircuit &gc,
    const std::vector<std::vector<gate_t>> &contribs,
    const std::vector<int> &members, const std::vector<long> &weights,
    const ProductDecomp &pd, bool &ok)
{
  const int nf = static_cast<int>(pd.parts.size());

  auto partOf = [&](int m, int f) {
    std::vector<gate_t> p;
    for (gate_t l : contribs[m])
      if (pd.leafFactor.at(l) == f) p.push_back(l);
    return p;
  };

  std::map<std::vector<std::vector<gate_t>>, long> grid;
  for (int m : members) {
    std::vector<std::vector<gate_t>> key(nf);
    for (int f = 0; f < nf; ++f) key[f] = partOf(m, f);
    grid[key] = weights[m];
  }

  int piv = -1;
  for (int m : members) if (weights[m] != 0) { piv = m; break; }
  if (piv < 0) { ok = false; return {}; }     /* all zero: additive handled it */
  const long D = weights[piv];
  std::vector<std::vector<gate_t>> ref(nf);
  for (int f = 0; f < nf; ++f) ref[f] = partOf(piv, f);

  /* Axis values A^f (aligned to pd.parts[f]) and a part -> index map. */
  std::vector<std::vector<long>> A(nf);
  std::vector<std::map<std::vector<gate_t>, int>> partIdx(nf);
  for (int f = 0; f < nf; ++f)
    for (const auto &p : pd.parts[f]) {
      partIdx[f][p] = static_cast<int>(A[f].size());
      std::vector<std::vector<gate_t>> key = ref;
      key[f] = p;
      auto it = grid.find(key);
      if (it == grid.end()) { ok = false; return {}; }
      A[f].push_back(it->second);
    }

  /* D^{nf-1}. */
  __int128 Dk1 = 1;
  for (int t = 0; t < nf - 1; ++t)
    if (!i128_mul(Dk1, static_cast<__int128>(D), Dk1)) { ok = false; return {}; }

  /* Verify multiplicative separability: ∏_f A^f_{p_f} == w_m · D^{nf-1}. */
  for (int m : members) {
    __int128 prod = 1;
    for (int f = 0; f < nf; ++f) {
      long a = A[f][partIdx[f].at(partOf(m, f))];
      if (!i128_mul(prod, static_cast<__int128>(a), prod)) { ok = false; return {}; }
    }
    __int128 rhs;
    if (!i128_mul(static_cast<__int128>(weights[m]), Dk1, rhs)) { ok = false; return {}; }
    if (prod != rhs) { ok = false; return {}; }      /* not multiplicatively separable */
  }

  /* Per-factor sum PMFs (over the axis weights), product-convolved. */
  std::map<__int128, double> run;
  run[1] = 1.0;                                       /* multiplicative identity */
  for (int f = 0; f < nf; ++f) {
    std::map<long, double> Pf = sumPMF(gc, pd.parts[f], A[f], ok);
    if (!ok) return {};
    std::map<__int128, double> nxt;
    for (const auto &rk : run)
      for (const auto &sk : Pf) {
        __int128 prod;
        if (!i128_mul(rk.first, static_cast<__int128>(sk.first), prod)) {
          ok = false; return {};
        }
        nxt[prod] += rk.second * sk.second;
      }
    if (nxt.size() > kMaxSumSupport) { ok = false; return {}; }
    run.swap(nxt);
  }

  /* Divide each product by D^{nf-1} (exact per world) and downcast. */
  std::map<long, double> out;
  for (const auto &kv : run) {
    if (kv.first % Dk1 != 0) { ok = false; return {}; }    /* defensive */
    __int128 bs = kv.first / Dk1;
    if (bs > static_cast<__int128>(LONG_MAX) ||
        bs < static_cast<__int128>(LONG_MIN)) { ok = false; return {}; }
    out[static_cast<long>(bs)] += kv.second;
  }
  return out;
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
        /* Cartesian product.  Tractable cases: (1) the value depends on a
         * single factor f -- SUM = S_f · M, with S_f the weighted sum over
         * factor f and M = ∏_{i≠f} N_i the count-product of the others (the
         * fast path below, detected by a weight constant within each f-part
         * group); (2) a branch-spanning but *additively separable* value
         * (sum(b+c)) -- per-factor joint (sum,count) distributions in
         * @c sumCountPMF; (3) a *multiplicatively separable* value (sum(b*c))
         * -- product of per-factor weighted sums in @c mulSeparableSumPMF
         * (both in the else arm).  A value that is none of these couples the
         * factors (may be #P-hard), so it bails. */
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
        if (chosen >= 0) {
          /* Single-factor value: SUM = S_f · M (the other factors
           * contribute only their count). */

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
        } else {
          /* Branch-spanning value.  Two tractable shapes: *additively*
           * separable (sum(b+c)) -> fold the per-factor joint (sum,count)
           * distributions (sumCountPMF) and read off the sum marginal;
           * *multiplicatively* separable (sum(b*c)) -> product of the
           * per-factor weighted sums (mulSeparableSumPMF).  Try additive
           * first (a value that is both is constant, handled there); a value
           * that is neither couples the factors and bails. */
          std::vector<std::vector<long>> sep;
          if (recoverAdditiveSeparation(contribs, members, weights, pd, sep)) {
            std::vector<std::vector<gate_t>> bc;
            std::vector<long> bw;
            bc.reserve(members.size());
            bw.reserve(members.size());
            for (int m : members) {
              bc.push_back(contribs[m]);
              bw.push_back(weights[m]);
            }
            JointPMF j = sumCountPMF(gc, std::move(bc), std::move(bw), ok);
            if (!ok) return {};
            for (const auto &kv : j) blockPMF[kv.first.first] += kv.second;
          } else {
            blockPMF = mulSeparableSumPMF(gc, contribs, members, weights, pd, ok);
            if (!ok) return {};               /* neither separable: bail */
          }
          if (blockPMF.size() > kMaxSumSupport) { ok = false; return {}; }
        }
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

    /* Parse every contributor: either a plain product of independent
     * @c gate_input leaves (TID, fed to the laminar / product recursion) or a
     * single @c gate_mulinput -- one alternative of a mutually-exclusive BID
     * block (e.g. @c repair_key), identified by its shared block-key child.
     * A contributor mixing the two (a join onto a BID row) or holding several
     * mulinputs is out of scope and bails to enumeration. */
    std::vector<std::vector<gate_t>> leaves;     /* TID contributor leaf sets */
    std::vector<long> tid_vals;                  /* per-TID value (match.ms, aligned) */
    /* block key -> alternatives (prob, value).  A BID block is a categorical:
     * at most one alternative present (Σp_i ≤ 1), the null arm contributes 0. */
    std::map<gate_t, std::vector<std::pair<double, long>>> blocks;
    for (std::size_t i = 0; i < n && ok; ++i) {
      if (gc.getGateType(ks[i]) == gate_mulinput) {
        const auto &ch = gc.getWires(ks[i]);
        if (ch.size() != 1) { ok = false; break; }   /* not a block alternative */
        blocks[ch[0]].push_back({gc.getProb(ks[i]),
                                 static_cast<long>(match.ms[i])});
      } else {
        std::vector<gate_t> ls;
        if (parseProductContributor(gc, ks[i], ls)) {
          leaves.push_back(std::move(ls));
          tid_vals.push_back(static_cast<long>(match.ms[i]));
        } else {
          /* Not a product (a UNION/EXCEPT contributor: gate_plus / gate_monus,
           * non-read-once on a shared base tuple).  Exact iff its footprint is
           * private -- then it is an independent event, modelled as a
           * one-alternative BID block of its exact marginal. */
          double pi;
          if (!contributorExactMarginal(gc, ks[i], ref, pi)) { ok = false; break; }
          blocks[ks[i]].push_back({pi, static_cast<long>(match.ms[i])});
        }
      }
    }
    if (!ok) continue;

    /* Independence guard: a block key (shared by its alternatives) must not
     * also surface as a TID leaf, which would couple the block to an
     * independent contributor.  Distinct repair_key blocks already get
     * distinct keys; cross-group sharing is caught by aggSubtreePrivate. */
    {
      std::set<gate_t> tidset;
      for (const auto &ls : leaves) tidset.insert(ls.begin(), ls.end());
      bool clash = false;
      for (const auto &b : blocks)
        if (tidset.count(b.first)) { clash = true; break; }
      if (clash) continue;
    }

    /* The cmp's randomness must be private to its agg subtree -- no gate
     * reachable from the agg referenced from outside it -- the soundness
     * precondition for resolving the cmp to an independent Bernoulli.
     * Subsumes the per-semimod ref==1 and per-leaf ref==cnt checks and
     * extends them to nested / shared product gates (subquery tuples). */
    if (!aggSubtreePrivate(gc, agg, ref)) continue;

    /* Σ_i p_i of a BID block (clamped). */
    auto blockMass = [](const std::vector<std::pair<double, long>> &alts) {
      double psum = 0.0;
      for (const auto &alt : alts) psum += alt.first;
      return psum > 1.0 ? 1.0 : psum;
    };

    /* Dispatch on the aggregate; each arm computes the exact probability
     * over the hierarchical (laminar) contributor structure, recursing
     * through shared root events.  A non-laminar shape clears @c ok and
     * the cmp falls back to exact enumeration. */
    double pr;
    if (agg_kind == AggregationOperator::COUNT) {
      /* countPMF treats every contributor as +1 (cardinality), correct only for
       * count(*) / count(col) with no NULLs.  A count(col) with NULL-valued
       * contributors carries per-row 0/1 values (match.ms), so cardinality would
       * over-count; defer it to the value-aware generic enumeration
       * (having_semantics -> sum_dp), which also keeps the scalar empty world. */
      bool all_one = true;
      for (int m : match.ms) if (m != 1) { all_one = false; break; }
      if (!all_one) continue;
      std::vector<double> total = countPMF(gc, leaves, ok);
      if (!ok) continue;
      /* Each BID block adds 0 or 1 to the count (mutual exclusion): present
       * w.p. Σp_i, absent w.p. 1-Σp_i; independent of the rest. */
      for (const auto &b : blocks) {
        double psum = blockMass(b.second);
        total = convolve(total, std::vector<double>{1.0 - psum, psum});
      }
      const bool is_scalar =
        (gc.getInfos(agg).second & PROVSQL_AGG_SCALAR_FLAG) != 0;
      pr = prFromPMF(total, match.op, match.C, is_scalar);
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
      auto shift = [&](long m) { return is_avg ? m - match.C : m; };
      std::vector<long> weights;
      weights.reserve(tid_vals.size());
      long lo = 0, hi = 0;
      for (long m : tid_vals) {
        long w = shift(m);
        weights.push_back(w);
        if (w < 0) lo += w; else hi += w;
      }
      for (const auto &b : blocks)
        for (const auto &alt : b.second) {
          long w = shift(alt.second);
          if (w < 0) lo += w; else hi += w;
        }
      /* Reachable-sum range cap (Remark 3 pseudo-polynomial caveat). */
      if (hi - lo + 1 > static_cast<long>(kMaxSumSupport)) continue;
      const long thr = is_avg ? 0 : match.C;

      std::map<long, double> dist = sumPMF(gc, leaves, std::move(weights), ok);
      if (!ok) continue;
      /* Convolve each BID block's categorical (shifted) sum distribution. */
      for (const auto &b : blocks) {
        std::map<long, double> bpmf;
        for (const auto &alt : b.second) bpmf[shift(alt.second)] += alt.first;
        bpmf[0] += 1.0 - blockMass(b.second);     /* null outcome: sum 0 */
        std::map<long, double> nd;
        for (const auto &a : dist)
          for (const auto &c : bpmf)
            nd[a.first + c.first] += a.second * c.second;
        if (nd.size() > kMaxSumSupport) { ok = false; break; }
        dist.swap(nd);
      }
      if (!ok) continue;
      pr = 0.0;
      for (const auto &kv : dist)
        if (sumSatisfies(kv.first, match.op, thr)) pr += kv.second;
      /* Exclude the empty group: its (shifted) sum is 0, so subtract its
       * mass when 0 satisfies the predicate (a non-empty group that
       * happens to sum to the threshold stays).  The empty world is all TID
       * contributors absent AND every block in its null outcome. */
      if (sumSatisfies(0, match.op, thr)) {
        double emptyMass = pAllAbsent(gc, leaves, ok);
        if (!ok) continue;
        for (const auto &b : blocks) emptyMass *= 1.0 - blockMass(b.second);
        pr -= emptyMass;
      }
    } else {  /* MIN or MAX */
      /* MIN/MAX over the TID part (laminar pAllAbsent) and the BID blocks
       * (each an independent categorical; a value-thresholded subset of a
       * block is all-absent w.p. 1-Σp over its matching alternatives). */
      std::vector<std::vector<std::pair<long, double>>> blockvec;
      blockvec.reserve(blocks.size());
      for (const auto &b : blocks) {
        std::vector<std::pair<long, double>> alts;
        alts.reserve(b.second.size());
        for (const auto &alt : b.second) alts.push_back({alt.second, alt.first});
        blockvec.push_back(std::move(alts));
      }
      pr = minMaxProb(gc, leaves, tid_vals, blockvec, agg_kind, match.op,
                      match.C, ok);
      if (!ok) continue;
    }

    if (pr < 0.0) pr = 0.0;
    if (pr > 1.0) pr = 1.0;

    gc.resolveCmpToBernoulli(cmp, pr);
    ++resolved;
  }

  return resolved;
}

double aggAvgRawMomentExact(GenericCircuit &gc, gate_t g, unsigned k,
                            bool &ok)
{
  ok = false;
  if (gc.getGateType(g) != gate_agg) return 0.0;

  /* Per-row (contributor leaf set, value) pairs from the semimod
   * children -- the same contributor parse the HAVING cmp path uses. */
  std::vector<std::vector<gate_t>> contribs;
  std::vector<double> values;
  for (gate_t sm : gc.getWires(g)) {
    if (gc.getGateType(sm) != gate_semimod) return 0.0;
    const auto &w = gc.getWires(sm);
    if (w.size() != 2) return 0.0;
    double v;
    try {
      v = parseDoubleStrict(gc.getExtra(w[1]));
    } catch (const CircuitException &) {
      return 0.0;                       /* non-numeric value: decline */
    }
    std::vector<gate_t> leaves;
    if (!parseProductContributor(gc, w[0], leaves))
      return 0.0;                       /* not a private product: decline */
    contribs.push_back(std::move(leaves));
    values.push_back(v);
  }
  for (const auto &c : contribs)
    for (gate_t l : c)
      if (std::isnan(gc.getProb(l))) return 0.0;   /* unset prob: decline */

  /* Joint (sum, count) distribution via the shared HAVING machinery
   * (double instantiation: independent rows fold directly, laminar
   * shared-root groups recurse; a non-laminar product block self-gates). */
  bool pmf_ok = true;
  JointPMFT<double> pmf = sumCountPMF(gc, std::move(contribs),
                                      std::move(values), pmf_ok);
  if (!pmf_ok) return 0.0;

  /* E[AVG^k | COUNT >= 1]: AVG over the empty world is NULL, so the
   * moment conditions on the aggregate being defined -- the same
   * convention as the MIN / MAX arms of agg_raw_moment. */
  double num = 0.0, den = 0.0;
  for (const auto &kv : pmf) {
    if (kv.first.second < 1) continue;
    den += kv.second;
    num += kv.second * std::pow(kv.first.first
                                  / static_cast<double>(kv.first.second),
                                static_cast<double>(k));
  }
  if (!(den > 1e-12)) return 0.0;       /* never defined: decline (the MC
                                           fallback then reports the same
                                           undefined answer) */
  ok = true;
  return num / den;
}

}  // namespace provsql
