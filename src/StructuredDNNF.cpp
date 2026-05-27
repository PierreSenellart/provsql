/**
 * @file StructuredDNNF.cpp
 * @brief Implementation of the reduced-OBDD builder (see StructuredDNNF.h).
 *
 * Standard bottom-up apply construction: each gate is compiled to a reduced
 * OBDD by combining its children's OBDDs (apply for AND/OR, terminal swap for
 * NOT), with a unique-table for reducedness and an apply-cache / negate-cache
 * for polynomial time.  Correctness of the memoisation is the textbook ROBDD
 * argument (canonical reduced OBDD per function + order); there is no bespoke
 * "atom-frontier" key to get wrong.  Size depends on the order: linear in the
 * lineage for an inversion-free query's Prop. 4.5 order, exponential for a bad
 * one.
 */
#include "StructuredDNNF.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace {
constexpr int FALSE_NODE = 0;
constexpr int TRUE_NODE  = 1;
constexpr int OP_AND = 0;
constexpr int OP_OR  = 1;
}

int StructuredDNNF::varOf(int node) const
{
  /* terminals sort below every variable */
  return node < 2 ? INT_MAX : nodes_[(std::size_t) node].var;
}

int StructuredDNNF::mk(int var, int lo, int hi)
{
  if (lo == hi)                         /* reduction rule: redundant test */
    return lo;
  auto key = std::make_tuple(var, lo, hi);
  auto it = unique_.find(key);
  if (it != unique_.end())
    return it->second;
  int id = (int) nodes_.size();
  nodes_.push_back(Node{var, lo, hi});
  unique_.emplace(key, id);
  return id;
}

int StructuredDNNF::applyOp(int op, int f, int g)
{
  /* terminal short-circuits */
  if (op == OP_AND) {
    if (f == FALSE_NODE || g == FALSE_NODE) return FALSE_NODE;
    if (f == TRUE_NODE) return g;
    if (g == TRUE_NODE) return f;
  } else { /* OP_OR */
    if (f == TRUE_NODE || g == TRUE_NODE) return TRUE_NODE;
    if (f == FALSE_NODE) return g;
    if (g == FALSE_NODE) return f;
  }
  if (f == g) return f;

  /* canonicalise operand order (AND/OR are commutative) for cache hits */
  if (f > g) std::swap(f, g);
  auto key = std::make_tuple(op, f, g);
  auto it = apply_cache_.find(key);
  if (it != apply_cache_.end())
    return it->second;

  int vf = varOf(f), vg = varOf(g);
  int v = vf < vg ? vf : vg;
  int f0 = (vf == v) ? nodes_[(std::size_t) f].lo : f;
  int f1 = (vf == v) ? nodes_[(std::size_t) f].hi : f;
  int g0 = (vg == v) ? nodes_[(std::size_t) g].lo : g;
  int g1 = (vg == v) ? nodes_[(std::size_t) g].hi : g;

  int lo = applyOp(op, f0, g0);
  int hi = applyOp(op, f1, g1);
  int res = mk(v, lo, hi);
  apply_cache_.emplace(key, res);
  return res;
}

int StructuredDNNF::negate(int f)
{
  if (f == FALSE_NODE) return TRUE_NODE;
  if (f == TRUE_NODE)  return FALSE_NODE;
  auto it = negate_cache_.find(f);
  if (it != negate_cache_.end())
    return it->second;
  int res = mk(nodes_[(std::size_t) f].var,
               negate(nodes_[(std::size_t) f].lo),
               negate(nodes_[(std::size_t) f].hi));
  negate_cache_.emplace(f, res);
  return res;
}

int StructuredDNNF::build(const BooleanCircuit &bc, gate_t g,
                          const std::map<gate_t,int> &rank,
                          std::map<gate_t,int> &memo)
{
  auto mit = memo.find(g);
  if (mit != memo.end())
    return mit->second;

  int res;
  switch (bc.getGateType(g)) {
    case BooleanGate::IN: {
      auto rit = rank.find(g);
      if (rit == rank.end())
        throw CircuitException("StructuredDNNF: input gate has no rank in the "
                               "supplied variable order");
      res = mk(rit->second, FALSE_NODE, TRUE_NODE);
      break;
    }
    case BooleanGate::AND: {
      res = TRUE_NODE;
      for (gate_t c : bc.getWires(g))
        res = applyOp(OP_AND, res, build(bc, c, rank, memo));
      break;
    }
    case BooleanGate::OR: {
      res = FALSE_NODE;
      for (gate_t c : bc.getWires(g))
        res = applyOp(OP_OR, res, build(bc, c, rank, memo));
      break;
    }
    case BooleanGate::NOT: {
      const auto &w = bc.getWires(g);
      if (w.size() != 1)
        throw CircuitException("StructuredDNNF: NOT gate must have one child");
      res = negate(build(bc, w[0], rank, memo));
      break;
    }
    default:
      throw CircuitException("StructuredDNNF: unsupported gate type "
                             "(multivalued input or non-Boolean gate)");
  }
  memo.emplace(g, res);
  return res;
}

StructuredDNNF::StructuredDNNF(const BooleanCircuit &bc, gate_t root,
                               const std::map<gate_t, int> &input_rank)
{
  if (bc.hasMultivaluedGates())
    throw CircuitException("StructuredDNNF: multivalued inputs (BID) are out "
                           "of scope for the inversion-free OBDD path");

  /* probability per rank */
  std::size_t ninputs = input_rank.size();
  prob_by_rank_.assign(ninputs, 0.0);
  for (const auto &kv : input_rank) {
    if (kv.second < 0 || (std::size_t) kv.second >= ninputs)
      throw CircuitException("StructuredDNNF: input rank out of range");
    prob_by_rank_[(std::size_t) kv.second] = bc.getProb(kv.first);
  }

  /* terminals */
  nodes_.push_back(Node{INT_MAX, 0, 0});   /* 0 = FALSE */
  nodes_.push_back(Node{INT_MAX, 0, 0});   /* 1 = TRUE  */

  std::map<gate_t,int> memo;
  root_ = build(bc, root, input_rank, memo);
}

std::size_t StructuredDNNF::size() const
{
  std::vector<char> seen(nodes_.size(), 0);
  std::vector<int> stack;
  std::size_t count = 0;
  if (root_ >= 2) stack.push_back(root_);
  while (!stack.empty()) {
    int n = stack.back(); stack.pop_back();
    if (n < 2 || seen[(std::size_t) n]) continue;
    seen[(std::size_t) n] = 1;
    ++count;
    stack.push_back(nodes_[(std::size_t) n].lo);
    stack.push_back(nodes_[(std::size_t) n].hi);
  }
  return count;
}

double StructuredDNNF::probability() const
{
  std::vector<double> cache(nodes_.size(), -1.0);
  cache[FALSE_NODE] = 0.0;
  cache[TRUE_NODE]  = 1.0;
  /* nodes are created children-before-parents, so a single backward sweep
   * over the node table evaluates every node after its lo/hi children */
  for (std::size_t i = 2; i < nodes_.size(); ++i) {
    const Node &n = nodes_[i];
    double p = prob_by_rank_[(std::size_t) n.var];
    cache[i] = p * cache[(std::size_t) n.hi] + (1.0 - p) * cache[(std::size_t) n.lo];
  }
  return cache[(std::size_t) root_];
}

/* ======================================================================== *
 *  StructuredDNNFBuilder: the §4 top-down structured-d-DNNF construction    *
 * ======================================================================== */

namespace {

/* Hard caps so a pathological input degrades to "no decomposition" / a thrown
 * size guard rather than running away.  These bound work, not correctness. */
constexpr std::size_t DNF_TERM_LIMIT    = 8'000'000;  // crossProduct blow-up
constexpr std::size_t DECOMP_VAR_LIMIT  = 1024;       // skip pairwise above this
/* Subsumption (O(T²)) and the decomposition cross-product check share this
 * threshold: both run on residuals at or below it, neither above.  Keeping them
 * paired matters for soundness -- the cross-product verification assumes the
 * term set is subsumption-free.  Large residuals near the top are handled by
 * Shannon decisions alone (correct, linear under the query order via caching). */
constexpr std::size_t DECOMP_TERM_LIMIT = 512;

inline bool contains(const std::vector<int> &term, int v)
{
  return std::binary_search(term.begin(), term.end(), v);
}

/* "unset" marker for the per-variable gate slots; distinct from any real gate
 * id (which are dense from 0). */
constexpr gate_t NO_GATE = gate_t{~std::size_t{0}};

}  // namespace

bool StructuredDNNFBuilder::CacheKey::operator==(const CacheKey &o) const
{
  return fs == o.fs && d == o.d;
}

std::size_t StructuredDNNFBuilder::CacheKeyHash::operator()(const CacheKey &k) const
{
  /* order-sensitive fold; DNF keys are canonical so equal DNFs hash equal */
  std::size_t h = 1469598103934665603ull;          // FNV-ish basis
  auto mix = [&](std::size_t x){ h ^= x + 0x9e3779b97f4a7c15ull + (h<<6) + (h>>2); };
  for (const auto &t : k.d) {
    for (int v : t) mix((std::size_t) (unsigned) v);
    mix(0xffffffffull);                             // term separator
  }
  mix((std::size_t) k.fs);
  return h;
}

StructuredDNNFBuilder::DNF
StructuredDNNFBuilder::canonical(DNF d)
{
  /* sort + dedup variables within each term */
  for (auto &t : d) {
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
  }
  /* an empty term is the constant TRUE: the whole DNF collapses */
  for (const auto &t : d)
    if (t.empty())
      return DNF{ Term{} };

  std::sort(d.begin(), d.end());
  d.erase(std::unique(d.begin(), d.end()), d.end());

  /* drop subsumed terms: if t' ⊇ t (some other term is a superset) t' is
   * redundant in a disjunction.  O(n²·k) in the term count, so only worth it on
   * the small residuals where it sharpens decomposition; large residuals (near
   * the top) are not decomposed anyway, and skipping keeps canonical O(n log n)
   * there.  Subsumption only ever removes terms, so skipping never changes the
   * function. */
  if (d.size() > DECOMP_TERM_LIMIT)
    return d;
  std::vector<char> dead(d.size(), 0);
  for (std::size_t i = 0; i < d.size(); ++i) {
    if (dead[i]) continue;
    for (std::size_t j = 0; j < d.size(); ++j) {
      if (i == j || dead[j] || d[i].size() > d[j].size())
        continue;
      bool subset = std::includes(d[j].begin(), d[j].end(),
                                  d[i].begin(), d[i].end());
      if (subset)                       /* d[i] ⊆ d[j]: d[j] is subsumed */
        dead[j] = 1;
    }
  }
  DNF out;
  for (std::size_t i = 0; i < d.size(); ++i)
    if (!dead[i])
      out.push_back(std::move(d[i]));
  return out;
}

StructuredDNNFBuilder::DNF
StructuredDNNFBuilder::condition(const DNF &d, Var v, bool value)
{
  DNF out;
  out.reserve(d.size());
  for (const auto &t : d) {
    bool has = contains(t, v);
    if (value) {                        /* v = true */
      if (!has) { out.push_back(t); continue; }
      Term nt;                          /* drop v; a now-empty term => TRUE */
      nt.reserve(t.size() - 1);
      for (int x : t) if (x != v) nt.push_back(x);
      out.push_back(std::move(nt));
    } else {                            /* v = false: terms needing v die */
      if (!has) out.push_back(t);
    }
  }
  return canonical(std::move(out));
}

std::vector<StructuredDNNFBuilder::DNF>
StructuredDNNFBuilder::andDecompose(const DNF &d) const
{
  /* variables present, ascending */
  std::set<int> vset;
  for (const auto &t : d) for (int v : t) vset.insert(v);
  std::vector<int> vars(vset.begin(), vset.end());
  const std::size_t V = vars.size(), T = d.size();
  if (V <= 1 || V > DECOMP_VAR_LIMIT || T > DECOMP_TERM_LIMIT)
    return { d };

  std::map<int, int> idx;               /* var -> 0..V-1 */
  for (std::size_t i = 0; i < V; ++i) idx[vars[i]] = (int) i;

  /* per-variable term count and pairwise co-occurrence count (n11) */
  std::vector<int> cnt(V, 0);
  std::map<std::pair<int,int>, int> co;
  for (const auto &t : d) {
    for (int v : t) cnt[(std::size_t) idx[v]]++;
    for (std::size_t a = 0; a < t.size(); ++a)
      for (std::size_t b = a + 1; b < t.size(); ++b) {
        int u = idx[t[a]], w = idx[t[b]];
        if (u > w) std::swap(u, w);
        co[{u, w}]++;
      }
  }

  /* union-find: variables u,w that are *not* independent must share a block.
   * |proj_u| = 1 + (cnt[u] < T)  (cnt[u] > 0 since u is present).
   * Independent iff |proj_{u,w}| == |proj_u|·|proj_w|. */
  std::vector<int> uf(V);
  for (std::size_t i = 0; i < V; ++i) uf[i] = (int) i;
  std::function<int(int)> find = [&](int x){ while (uf[x]!=x){ uf[x]=uf[uf[x]]; x=uf[x]; } return x; };
  auto unite = [&](int a, int b){ a=find(a); b=find(b); if(a!=b) uf[a]=b; };

  auto projcard = [&](int u){ return 1 + (cnt[(std::size_t)u] < (int) T ? 1 : 0); };
  for (std::size_t a = 0; a < V; ++a)
    for (std::size_t b = a + 1; b < V; ++b) {
      int u = (int) a, w = (int) b;
      int n11 = 0;
      auto it = co.find({u, w});
      if (it != co.end()) n11 = it->second;
      int n10 = cnt[a] - n11, n01 = cnt[b] - n11;
      int n00 = (int) T - cnt[a] - cnt[b] + n11;
      int card = (n11>0) + (n10>0) + (n01>0) + (n00>0);
      if (card != projcard(u) * projcard(w))   /* dependent */
        unite(u, w);
    }

  /* gather blocks */
  std::map<int, std::vector<int>> blocks;     /* root -> vars (original ids) */
  for (std::size_t i = 0; i < V; ++i)
    blocks[find((int) i)].push_back(vars[i]);
  if (blocks.size() <= 1)
    return { d };

  /* verify the exact cross-product: |T| == ∏_b |proj_b(d)|.  Since every term
   * projects into each block, T ⊆ ×proj always; equal cardinality ⟺ equality,
   * so this makes the split sound (the AND node is genuinely decomposable). */
  std::vector<std::set<int>> blockset;        /* fast membership per block */
  std::vector<std::vector<int>> blockvars;
  for (auto &kv : blocks) {
    blockset.emplace_back(kv.second.begin(), kv.second.end());
    blockvars.push_back(kv.second);
  }
  std::vector<std::set<Term>> proj(blockset.size());
  for (const auto &t : d)
    for (std::size_t bi = 0; bi < blockset.size(); ++bi) {
      Term p;
      for (int v : t) if (blockset[bi].count(v)) p.push_back(v);
      proj[bi].insert(std::move(p));
    }
  std::size_t prod = 1;
  for (const auto &s : proj) prod *= s.size();
  if (prod != T)
    return { d };                             /* not an exact product */

  std::vector<DNF> parts;
  for (auto &s : proj) {
    DNF part(s.begin(), s.end());
    parts.push_back(canonical(std::move(part)));
  }
  return parts;
}

StructuredDNNFBuilder::DNF
StructuredDNNFBuilder::extract(const BooleanCircuit &bc, gate_t g,
                               const std::map<gate_t, int> &rank,
                               std::map<gate_t, DNF> &memo) const
{
  auto mit = memo.find(g);
  if (mit != memo.end()) return mit->second;

  DNF res;
  switch (bc.getGateType(g)) {
    case BooleanGate::IN: {
      auto rit = rank.find(g);
      if (rit == rank.end())
        throw CircuitException("StructuredDNNFBuilder: input gate has no rank "
                               "in the supplied variable order");
      res = DNF{ Term{ rit->second } };
      break;
    }
    case BooleanGate::AND: {
      res = DNF{ Term{} };                      /* neutral element: TRUE */
      for (gate_t c : bc.getWires(g)) {
        DNF cd = extract(bc, c, rank, memo);
        DNF prod;
        if (res.size() * cd.size() > DNF_TERM_LIMIT)
          throw CircuitException("StructuredDNNFBuilder: DNF expansion exceeds "
                                 "size guard");
        prod.reserve(res.size() * cd.size());
        for (const auto &ta : res)
          for (const auto &tb : cd) {
            Term t = ta;
            t.insert(t.end(), tb.begin(), tb.end());
            prod.push_back(std::move(t));
          }
        res = canonical(std::move(prod));
      }
      break;
    }
    case BooleanGate::OR: {
      for (gate_t c : bc.getWires(g)) {
        DNF cd = extract(bc, c, rank, memo);
        for (auto &t : cd) res.push_back(std::move(t));
        if (res.size() > DNF_TERM_LIMIT)
          throw CircuitException("StructuredDNNFBuilder: DNF expansion exceeds "
                                 "size guard");
      }
      res = canonical(std::move(res));
      break;
    }
    default:
      throw CircuitException("StructuredDNNFBuilder: unsupported gate type "
                             "(NOT, multivalued input or non-Boolean gate)");
  }
  memo.emplace(g, res);
  return res;
}

gate_t StructuredDNNFBuilder::newGate(BooleanGate type)
{
  gate_t g = dd_.setGate(type);
  if (max_nodes_ && dd_.getNbGates() > max_nodes_)
    throw CircuitException("StructuredDNNFBuilder: size guard exceeded");
  return g;
}

gate_t StructuredDNNFBuilder::mkLit(Var v)
{
  if (in_gate_[(std::size_t) v] == NO_GATE)
    in_gate_[(std::size_t) v] = dd_.setGate(BooleanGate::IN, prob_[(std::size_t) v]);
  return in_gate_[(std::size_t) v];
}

gate_t StructuredDNNFBuilder::mkNeg(Var v)
{
  if (not_gate_[(std::size_t) v] == NO_GATE) {
    gate_t n = newGate(BooleanGate::NOT);
    dd_.addWire(n, mkLit(v));
    not_gate_[(std::size_t) v] = n;
  }
  return not_gate_[(std::size_t) v];
}

gate_t StructuredDNNFBuilder::mkAnd(const std::vector<gate_t> &children)
{
  std::vector<gate_t> real;
  for (gate_t c : children) {
    if (c == false_gate_) return false_gate_;   /* absorbing */
    if (c == true_gate_)  continue;             /* neutral */
    real.push_back(c);
  }
  if (real.empty())      return true_gate_;
  if (real.size() == 1)  return real[0];
  gate_t g = newGate(BooleanGate::AND);
  for (gate_t c : real) dd_.addWire(g, c);
  return g;
}

std::vector<StructuredDNNFBuilder::DNF>
StructuredDNNFBuilder::orDecompose(const DNF &d) const
{
  /* variables present, with a dense index for union-find */
  std::set<int> vset;
  for (const auto &t : d) for (int v : t) vset.insert(v);
  if (vset.size() <= 1) return { d };
  std::vector<int> vars(vset.begin(), vset.end());
  std::map<int,int> idx;
  for (std::size_t i = 0; i < vars.size(); ++i) idx[vars[i]] = (int) i;

  std::vector<int> uf(vars.size());
  for (std::size_t i = 0; i < uf.size(); ++i) uf[i] = (int) i;
  std::function<int(int)> find = [&](int x){ while (uf[x]!=x){ uf[x]=uf[uf[x]]; x=uf[x]; } return x; };
  /* variables co-occurring in a term are connected (the disjunct couples them) */
  for (const auto &t : d)
    for (std::size_t a = 1; a < t.size(); ++a) {
      int r0 = find(idx[t[0]]), ra = find(idx[t[a]]);
      if (r0 != ra) uf[r0] = ra;
    }

  /* group terms by their component (all of a term's vars share one root) */
  std::map<int, DNF> comp;
  for (const auto &t : d) comp[find(idx[t[0]])].push_back(t);
  if (comp.size() <= 1) return { d };

  /* order components by least variable, so the chain follows the order */
  std::vector<std::pair<int,DNF>> ordered;
  for (auto &kv : comp) {
    int mn = INT_MAX;
    for (const auto &t : kv.second) mn = std::min(mn, t.front());
    ordered.push_back({ mn, std::move(kv.second) });
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &a, const auto &b){ return a.first < b.first; });
  std::vector<DNF> out;
  out.reserve(ordered.size());
  for (auto &pr : ordered) out.push_back(std::move(pr.second));
  return out;
}

gate_t StructuredDNNFBuilder::build(const DNF &draw, gate_t false_sink)
{
  DNF d = canonical(draw);
  if (d.empty())                                return false_sink;
  if (d.size() == 1 && d[0].empty())            return true_gate_;

  CacheKey key{ d, false_sink };
  auto cit = cache_.find(key);
  if (cit != cache_.end()) return cit->second;

  gate_t res;

  /* (1) OR-chain over variable-disjoint components.  d = c0 ∨ c1 ∨ … ∨ sink, the
   * cᵢ ordered by least variable.  Build right to left: each component's FALSE
   * leaf is the node for "everything later", so an inert component is one shared
   * node, never copied into the residual DNF. */
  std::vector<DNF> comps = orDecompose(d);
  if (comps.size() > 1) {
    gate_t tail = false_sink;
    for (auto it = comps.rbegin(); it != comps.rend(); ++it)
      tail = build(*it, tail);
    res = tail;
  } else {
    /* (2) decomposable AND: residual is a product of variable-disjoint factors.
     * Only when there is no pending OR tail (false_sink is the global FALSE):
     * with a tail, the function is (∏ factors) ∨ tail, which is not a clean
     * decomposable AND.  This also splits a single multi-variable term into a
     * flat AND of literal leaves. */
    std::vector<DNF> parts = (false_sink == false_gate_)
                               ? andDecompose(d) : std::vector<DNF>{ d };
    if (parts.size() > 1) {
      std::vector<gate_t> ch;
      ch.reserve(parts.size());
      for (const auto &p : parts) ch.push_back(build(p, false_gate_));
      res = mkAnd(ch);
    } else {
      /* (3) deterministic OR: Shannon-decide the lowest-rank variable present. */
      Var v = d[0][0];
      for (const auto &t : d) if (t[0] < v) v = t[0];
      gate_t ghi = build(condition(d, v, true), false_sink);
      gate_t glo = build(condition(d, v, false), false_sink);
      if (ghi == glo) {
        res = ghi;
      } else {
        gate_t andHi = mkAnd({ mkLit(v), ghi });
        gate_t andLo = mkAnd({ mkNeg(v), glo });
        gate_t orG = newGate(BooleanGate::OR);
        if (andHi != false_gate_) dd_.addWire(orG, andHi);
        if (andLo != false_gate_) dd_.addWire(orG, andLo);
        res = orG;
      }
    }
  }

  cache_.emplace(CacheKey{ std::move(d), false_sink }, res);
  return res;
}

StructuredDNNFBuilder::StructuredDNNFBuilder(const BooleanCircuit &bc,
                                             gate_t root,
                                             const std::map<gate_t, int> &input_rank,
                                             std::size_t max_nodes)
  : max_nodes_(max_nodes)
{
  if (bc.hasMultivaluedGates())
    throw CircuitException("StructuredDNNFBuilder: multivalued inputs (BID) are "
                           "out of scope for the inversion-free path");

  std::size_t ninputs = input_rank.size();
  prob_.assign(ninputs, 0.0);
  in_gate_.assign(ninputs, NO_GATE);
  not_gate_.assign(ninputs, NO_GATE);
  for (const auto &kv : input_rank) {
    if (kv.second < 0 || (std::size_t) kv.second >= ninputs)
      throw CircuitException("StructuredDNNFBuilder: input rank out of range");
    prob_[(std::size_t) kv.second] = bc.getProb(kv.first);
  }

  true_gate_  = dd_.setGate(BooleanGate::AND);   /* empty AND  = TRUE  */
  false_gate_ = dd_.setGate(BooleanGate::OR);    /* empty OR   = FALSE */

  std::map<gate_t, DNF> memo;
  DNF top = extract(bc, root, input_rank, memo);

  root_ = build(top, false_gate_);
  dd_.root = root_;
  dd_.simplify();
}

std::size_t StructuredDNNFBuilder::size() const
{
  std::set<gate_t> seen;
  std::vector<gate_t> stack{ dd_.root };
  while (!stack.empty()) {
    gate_t g = stack.back(); stack.pop_back();
    if (!seen.insert(g).second) continue;
    for (gate_t c : dd_.getWires(g)) stack.push_back(c);
  }
  return seen.size();
}
