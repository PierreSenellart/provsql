/**
 * @file DTree.cpp
 * @brief Implementation of the d-tree anytime interval-bounds engine.
 */
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// Keep the Boost-including headers ahead of the PostgreSQL block: port.h
// #defines snprintf to pg_snprintf, which breaks Boost headers (>= 1.79)
// whose inline code calls std::snprintf (boost/assert/source_location.hpp).
#include "BooleanCircuit.h"
#include "DTree.h"

extern "C" {
#include "provsql_utils.h" // provsql_interrupted
#include "miscadmin.h"     // check_stack_depth
}

namespace provsql {

namespace {

using Clauses = std::vector<std::set<gate_t> >;

/**
 * @brief Order-sensitive hash of a @e canonical clause set.
 *
 * The memo only ever sees clause sets that @c recurse has already
 * subsumption-reduced and sorted, so two logically equal residual DNFs have an
 * identical @c Clauses representation (same clause order, each clause a sorted
 * @c std::set) and therefore hash identically.  Collisions fall back to the
 * vector/set @c operator== the @c unordered_map applies, so the hash needs only
 * to be well-distributed, not perfect -- the lookup stays exact.  This replaces
 * the former @c std::map whose @c O(log n) lookups each ran a lexicographic
 * compare over @c vector<set<gate_t>> (the costly per-node key op flagged in the
 * d-tree TODO); the hash makes lookups average @c O(clause-set size).
 */
struct ClausesHash {
  std::size_t operator()(const Clauses &cls) const
  {
    std::size_t h = 1469598103934665603ull; // FNV-1a offset basis
    std::hash<gate_t> gh;
    for(const auto &cl : cls) {
      std::size_t ch = 1099511628211ull;    // per-clause FNV prime seed
      for(gate_t v : cl)
        ch = ch * 31u + gh(v);
      // boost::hash_combine-style mix so clause order matters and the
      // per-clause hashes do not simply XOR-cancel.
      h ^= ch + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    }
    return h;
  }
};

/**
 * @brief Shared recursion state: the circuit and the subproblem memo.
 *
 * The memo turns the Shannon/independence recursion from a tree into a shared
 * DAG: distinct paths that reach the same residual DNF (very common on path- and
 * cycle-shaped lineage) are computed once.  Without it the recursion is
 * exponential even on bounded treewidth; with it, the number of distinct
 * residual clause sets is polynomial there, matching the paper's tractable-query
 * behaviour.
 *
 * The key is the @e canonical clause set (subsumption-reduced, then sorted), and
 * the value is the subproblem's @b exact probability.  An entry is therefore
 * sound to reuse for @e any request (an exact value satisfies any width target),
 * but it is only ever WRITTEN on an exact request (@c max_width == 0), where the
 * whole recursion is exact -- an early-stopped interval is budget-dependent and
 * must not be cached.  Hashed (@c ClausesHash) for average-constant lookup.
 */
struct DTreeContext {
  const BooleanCircuit &c;
  std::unordered_map<Clauses, double, ClausesHash> memo;
};

/**
 * @brief Drop subsumed clauses from a monotone DNF.
 *
 * For a monotone DNF a clause is the conjunction of its (positive) literals, so
 * if clause @c A is a subset of clause @c B then @c B implies @c A and
 * @c A∨B ≡ A: the superset @c B is redundant.  Keeping only the minimal clauses
 * (and a single copy of duplicates) preserves the function and shrinks the work
 * Shannon expansion generates.  @c O(m^2) set-inclusion tests.
 */
void removeSubsumed(Clauses &clauses)
{
  // Ascending size, so a subsumer (subset) is always considered before any
  // clause it could subsume.
  std::sort(clauses.begin(), clauses.end(),
            [](const std::set<gate_t> &a, const std::set<gate_t> &b) {
              return a.size() < b.size();
            });
  Clauses kept;
  kept.reserve(clauses.size());
  for(auto &c : clauses) {
    bool subsumed = false;
    for(const auto &k : kept)
      // k ⊆ c  (k has size <= c by the sort), so c is subsumed by k.
      if(std::includes(c.begin(), c.end(), k.begin(), k.end())) {
        subsumed = true;
        break;
      }
    if(!subsumed)
      kept.push_back(std::move(c));
  }
  clauses = std::move(kept);
}

/**
 * @brief Partition a DNF into independent-or components.
 *
 * Two clauses are connected when their supports share an input leaf; the
 * connected components are sub-DNFs over disjoint variable sets, so the whole
 * DNF is their independent disjunction.  A single component means the
 * disjunction does not split here.  Union-find over the clause/variable
 * incidence, @c O(m·avg-clause·α).
 */
std::vector<Clauses> components(const Clauses &clauses)
{
  const size_t m = clauses.size();
  std::vector<size_t> parent(m);
  for(size_t i = 0; i < m; ++i)
    parent[i] = i;
  std::function<size_t(size_t)> find = [&](size_t x) {
    while(parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  std::unordered_map<gate_t, size_t> owner; // first clause seen carrying a leaf
  for(size_t i = 0; i < m; ++i)
    for(gate_t v : clauses[i]) {
      auto it = owner.find(v);
      if(it == owner.end())
        owner.emplace(v, i);
      else
        parent[find(i)] = find(it->second);
    }
  // Group clauses by component root, keeping a deterministic order.
  std::map<size_t, Clauses> groups;
  for(size_t i = 0; i < m; ++i)
    groups[find(i)].push_back(clauses[i]);
  std::vector<Clauses> out;
  out.reserve(groups.size());
  for(auto &kv : groups)
    out.push_back(std::move(kv.second));
  return out;
}

/// The variable appearing in the most clauses (ties broken by smallest gate id
/// for reproducibility) -- the Shannon-expansion pivot.
gate_t mostFrequentVar(const Clauses &clauses)
{
  std::map<gate_t, size_t> freq; // ordered for a deterministic tie-break
  for(const auto &c : clauses)
    for(gate_t v : c)
      ++freq[v];
  gate_t best{};
  size_t best_count = 0;
  bool found = false;
  for(const auto &kv : freq)
    if(!found || kv.second > best_count) { // strict '>' keeps the smallest id
      best = kv.first;
      best_count = kv.second;
      found = true;
    }
  return best;
}

DTreeInterval recurse(DTreeContext &ctx, Clauses clauses, double max_width)
{
  check_stack_depth();
  if(provsql_interrupted)
    throw CircuitException("Interrupted");

  if(clauses.empty())
    return {0., 0.}; // empty disjunction is false
  for(const auto &cl : clauses)
    if(cl.empty())
      return {1., 1.}; // a clause with no literals is true, so the DNF is true

  // Canonicalise (subsumption-reduce, then sort) so equivalent residual DNFs
  // share a memo key.
  removeSubsumed(clauses);
  std::sort(clauses.begin(), clauses.end());

  // An exact request keeps max_width == 0 through every decomposition (the ⊗
  // split passes 0/k = 0, ⊕ passes it unchanged), so the whole recursion is
  // exact and every result is cacheable; an approximate request never writes.
  const bool exact = (max_width <= 0.);
  if(exact) {
    const auto it = ctx.memo.find(clauses);
    if(it != ctx.memo.end())
      return {it->second, it->second};
  }

  // Cheap certified interval; stop as soon as it is narrow enough.
  double L, U;
  ctx.c.dnfBounds(clauses, L, U);
  if(U - L <= max_width) {
    if(exact)
      ctx.memo.emplace(clauses, L); // independent leaf: L == U, exact
    return {L, U};
  }

  DTreeInterval res;
  // Independent-or: recurse on each component with a 1/k share of the budget
  // (the OR width is at most the sum of component widths) and combine.
  std::vector<Clauses> comps = components(clauses);
  if(comps.size() > 1) {
    double prod_lower = 1., prod_upper = 1.;
    const double sub_width = max_width / static_cast<double>(comps.size());
    for(auto &comp : comps) {
      DTreeInterval r = recurse(ctx, std::move(comp), sub_width);
      prod_lower *= (1. - r.lower);
      prod_upper *= (1. - r.upper);
    }
    res = {1. - prod_lower, 1. - prod_upper};
  } else {
    // Shannon expansion on the most frequent variable x: Pr = Pr[x]·Pr[Phi|x=1]
    // + (1-Pr[x])·Pr[Phi|x=0].  For a monotone DNF the cofactors are, on x=1,
    // every clause with x stripped of x (the literal is satisfied) and every
    // clause without x unchanged; on x=0, only the clauses not containing x
    // survive.  The same width budget passes to both branches (the mixture
    // width is at most the larger branch's).
    const gate_t x = mostFrequentVar(clauses);
    const double px = ctx.c.getProb(x);
    Clauses pos, neg;
    pos.reserve(clauses.size());
    neg.reserve(clauses.size());
    for(const auto &cl : clauses) {
      if(cl.count(x)) {
        std::set<gate_t> reduced = cl;
        reduced.erase(x);
        pos.push_back(std::move(reduced));
      } else {
        pos.push_back(cl);
        neg.push_back(cl);
      }
    }
    const DTreeInterval rp = recurse(ctx, std::move(pos), max_width);
    const DTreeInterval rn = recurse(ctx, std::move(neg), max_width);
    res = {px * rp.lower + (1. - px) * rn.lower,
           px * rp.upper + (1. - px) * rn.upper};
  }

  if(exact)
    ctx.memo.emplace(clauses, res.lower); // exact (res.lower == res.upper)
  return res;
}

} // namespace

DTreeInterval dtreeBounds(const BooleanCircuit &c, Clauses clauses,
                          double max_width)
{
  DTreeContext ctx{c, {}};
  return recurse(ctx, std::move(clauses), max_width);
}

// ===========================================================================
// General-circuit d-tree: the same anytime engine on the BooleanCircuit DAG
// (AND / OR / NOT / IN), so it drops the monotone-DNF shape restriction.
// ===========================================================================

namespace {

using Assignment = std::unordered_map<gate_t, bool>;

/// Static cone of genuine variable inputs (probability strictly in (0,1)); the
/// memo + the recursion state for the general-circuit d-tree.
struct GenContext {
  const BooleanCircuit &c;
  std::unordered_map<gate_t, std::set<gate_t> > footprint;
  std::unordered_map<std::string, double> exactMemo; // exact subproblem values
};

inline unsigned long gid(gate_t g)
{
  return static_cast<unsigned long>(
    static_cast<std::underlying_type<gate_t>::type>(g));
}

/// Variable-input cone of @p g (inputs with probability in (0,1)).  Constant
/// inputs (probability 0 / 1) carry no variable and are dropped.  Throws on a
/// multivalued / undetermined gate so the caller falls back to another method.
/// unordered_map references survive rehash, so holding a child's reference while
/// inserting the parent is safe.
const std::set<gate_t> &footprintOf(GenContext &ctx, gate_t g)
{
  auto it = ctx.footprint.find(g);
  if(it != ctx.footprint.end())
    return it->second;
  std::set<gate_t> s;
  switch(ctx.c.getGateType(g)) {
  case BooleanGate::IN: {
    const double p = ctx.c.getProb(g);
    if(p > 0.0 && p < 1.0)
      s.insert(g);
    break;
  }
  case BooleanGate::AND:
  case BooleanGate::OR:
  case BooleanGate::NOT:
    for(gate_t ch : ctx.c.getWires(g)) {
      const auto &cs = footprintOf(ctx, ch);
      s.insert(cs.begin(), cs.end());
    }
    break;
  default: // MULIN / MULVAR / UNDETERMINED
    throw CircuitException(
      "d-tree: multivalued / undetermined gate not supported on the general "
      "circuit path");
  }
  return ctx.footprint.emplace(g, std::move(s)).first->second;
}

/// Whether every variable in @p g's cone is assigned by @p A (so @p g has a
/// definite truth value).
bool determined(GenContext &ctx, gate_t g, const Assignment &A)
{
  for(gate_t v : footprintOf(ctx, g))
    if(A.find(v) == A.end())
      return false;
  return true;
}

/// Truth value of a fully-determined gate under @p A.
bool evalDet(GenContext &ctx, gate_t g, const Assignment &A)
{
  switch(ctx.c.getGateType(g)) {
  case BooleanGate::IN: {
    auto it = A.find(g);
    if(it != A.end())
      return it->second;
    return ctx.c.getProb(g) >= 1.0; // a constant input (prob 0 or 1)
  }
  case BooleanGate::NOT:
    return !evalDet(ctx, ctx.c.getWires(g)[0], A);
  case BooleanGate::AND:
    for(gate_t ch : ctx.c.getWires(g))
      if(!evalDet(ctx, ch, A))
        return false;
    return true;
  case BooleanGate::OR:
    for(gate_t ch : ctx.c.getWires(g))
      if(evalDet(ctx, ch, A))
        return true;
    return false;
  default:
    throw CircuitException("d-tree: unsupported gate in evalDet");
  }
}

/// Partition @p live into groups whose free-variable footprints are pairwise
/// disjoint (independent sub-formulas), preserving a deterministic order.
std::vector<std::vector<gate_t> > genComponents(
  GenContext &ctx, const std::vector<gate_t> &live, const Assignment &A)
{
  const size_t m = live.size();
  std::vector<size_t> parent(m);
  for(size_t i = 0; i < m; ++i)
    parent[i] = i;
  std::function<size_t(size_t)> find = [&](size_t x) {
    while(parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
  };
  std::unordered_map<gate_t, size_t> owner; // first live index carrying a free var
  for(size_t i = 0; i < m; ++i)
    for(gate_t v : footprintOf(ctx, live[i])) {
      if(A.find(v) != A.end())
        continue; // assigned: not a shared free variable
      auto it = owner.find(v);
      if(it == owner.end())
        owner.emplace(v, i);
      else
        parent[find(i)] = find(it->second);
    }
  std::map<size_t, std::vector<gate_t> > groups;
  for(size_t i = 0; i < m; ++i)
    groups[find(i)].push_back(live[i]);
  std::vector<std::vector<gate_t> > out;
  out.reserve(groups.size());
  for(auto &kv : groups)
    out.push_back(std::move(kv.second));
  return out;
}

/// The free variable shared by the most members of @p live (ties -> smallest
/// gate id), the Shannon-expansion pivot.
gate_t genPivot(GenContext &ctx, const std::vector<gate_t> &live,
                const Assignment &A)
{
  std::map<gate_t, size_t> freq; // ordered for a deterministic tie-break
  for(gate_t c : live)
    for(gate_t v : footprintOf(ctx, c))
      if(A.find(v) == A.end())
        ++freq[v];
  gate_t best{};
  size_t best_count = 0;
  bool found = false;
  for(const auto &kv : freq)
    if(!found || kv.second > best_count) {
      best = kv.first;
      best_count = kv.second;
      found = true;
    }
  return best;
}

DTreeInterval genBound(GenContext &ctx, gate_t g, const Assignment &A);

/// Cheap sound interval of @c op over @p children under @p A: independent
/// components compose exactly; within a component AND uses a Bonferroni lower /
/// min upper and OR a max lower / union upper.  Generalises @c dnfBounds.
DTreeInterval genBoundGroup(GenContext &ctx, BooleanGate op,
                            const std::vector<gate_t> &children,
                            const Assignment &A)
{
  auto comps = genComponents(ctx, children, A);
  double L = (op == BooleanGate::AND) ? 1.0 : 1.0; // AND: prod L; OR: prod (1-L)
  double U = 1.0;
  for(const auto &comp : comps) {
    double gL, gU;
    if(comp.size() == 1) {
      DTreeInterval b = genBound(ctx, comp[0], A);
      gL = b.lower;
      gU = b.upper;
    } else if(op == BooleanGate::AND) {
      double sumL = 0.0;
      gU = 1.0;
      for(gate_t c : comp) {
        DTreeInterval b = genBound(ctx, c, A);
        sumL += b.lower;
        gU = std::min(gU, b.upper);
      }
      gL = sumL - (static_cast<double>(comp.size()) - 1.0); // Bonferroni
      if(gL < 0.0) gL = 0.0;
    } else { // OR
      double sumU = 0.0;
      gL = 0.0;
      for(gate_t c : comp) {
        DTreeInterval b = genBound(ctx, c, A);
        sumU += b.upper;
        gL = std::max(gL, b.lower);
      }
      gU = (sumU > 1.0) ? 1.0 : sumU; // union bound
    }
    if(op == BooleanGate::AND) { L *= gL; U *= gU; }
    else { L *= (1.0 - gL); U *= (1.0 - gU); }
  }
  if(op == BooleanGate::AND)
    return {L, U};
  return {1.0 - L, 1.0 - U};
}

DTreeInterval genBound(GenContext &ctx, gate_t g, const Assignment &A)
{
  switch(ctx.c.getGateType(g)) {
  case BooleanGate::IN: {
    auto it = A.find(g);
    if(it != A.end())
      return {it->second ? 1.0 : 0.0, it->second ? 1.0 : 0.0};
    double p = ctx.c.getProb(g);
    if(p < 0.0) p = 0.0;
    if(p > 1.0) p = 1.0;
    return {p, p};
  }
  case BooleanGate::NOT: {
    DTreeInterval b = genBound(ctx, ctx.c.getWires(g)[0], A);
    return {1.0 - b.upper, 1.0 - b.lower};
  }
  case BooleanGate::AND:
  case BooleanGate::OR:
    return genBoundGroup(ctx, ctx.c.getGateType(g), ctx.c.getWires(g), A);
  default:
    throw CircuitException("d-tree: unsupported gate in genBound");
  }
}

/// Key identifying an exact subproblem: the (op, child set, assignment over the
/// children's footprint) it depends on.  @p single distinguishes a one-gate
/// problem from a group with the same id.
std::string exactKey(GenContext &ctx, char tag, BooleanGate op,
                     const std::vector<gate_t> &gates, const Assignment &A)
{
  std::string k(1, tag);
  k += (op == BooleanGate::AND) ? 'A' : 'O';
  std::set<gate_t> footunion;
  for(gate_t g : gates) {
    k += ':';
    k += std::to_string(gid(g));
    const auto &fp = footprintOf(ctx, g);
    footunion.insert(fp.begin(), fp.end());
  }
  k += '|';
  for(gate_t v : footunion) { // std::set: ascending, canonical
    auto it = A.find(v);
    if(it != A.end()) {
      k += std::to_string(gid(v));
      k += it->second ? '=' : '#';
    }
  }
  return k;
}

DTreeInterval genRefineGroup(GenContext &ctx, BooleanGate op,
                             const std::vector<gate_t> &children,
                             Assignment &A, double w);

DTreeInterval genRefine(GenContext &ctx, gate_t g, Assignment &A, double w)
{
  check_stack_depth();
  if(provsql_interrupted)
    throw CircuitException("Interrupted");

  if(determined(ctx, g, A)) {
    double v = evalDet(ctx, g, A) ? 1.0 : 0.0;
    return {v, v};
  }
  if(w > 0.0) {
    DTreeInterval b = genBound(ctx, g, A);
    if(b.upper - b.lower <= w)
      return b;
  }
  switch(ctx.c.getGateType(g)) {
  case BooleanGate::NOT: {
    DTreeInterval r = genRefine(ctx, ctx.c.getWires(g)[0], A, w);
    return {1.0 - r.upper, 1.0 - r.lower};
  }
  case BooleanGate::IN: {
    const double p = ctx.c.getProb(g); // a single free variable
    return {p, p};
  }
  case BooleanGate::AND:
  case BooleanGate::OR:
    return genRefineGroup(ctx, ctx.c.getGateType(g), ctx.c.getWires(g), A, w);
  default:
    throw CircuitException("d-tree: unsupported gate in genRefine");
  }
}

DTreeInterval genRefineGroup(GenContext &ctx, BooleanGate op,
                             const std::vector<gate_t> &children,
                             Assignment &A, double w)
{
  check_stack_depth();
  if(provsql_interrupted)
    throw CircuitException("Interrupted");

  // Drop children fixed by A (and short-circuit on an absorbing one).
  std::vector<gate_t> live;
  live.reserve(children.size());
  for(gate_t c : children) {
    if(determined(ctx, c, A)) {
      bool v = evalDet(ctx, c, A);
      if(op == BooleanGate::AND && !v) return {0.0, 0.0};
      if(op == BooleanGate::OR && v) return {1.0, 1.0};
      // AND-true / OR-false: the identity, drop it
    } else {
      live.push_back(c);
    }
  }
  if(live.empty())
    return (op == BooleanGate::AND) ? DTreeInterval{1.0, 1.0}
                                    : DTreeInterval{0.0, 0.0};

  const bool exact = (w <= 0.0);
  std::sort(live.begin(), live.end());
  std::string key;
  if(exact) {
    key = exactKey(ctx, 'G', op, live, A);
    auto it = ctx.exactMemo.find(key);
    if(it != ctx.exactMemo.end())
      return {it->second, it->second};
  }

  DTreeInterval res;
  auto comps = genComponents(ctx, live, A);
  if(comps.size() > 1) {
    const double w_sub = w / static_cast<double>(comps.size());
    double L = 1.0, U = 1.0; // AND: prod; OR: prod of (1-.)
    for(auto &comp : comps) {
      DTreeInterval r = genRefineGroup(ctx, op, comp, A, w_sub);
      if(op == BooleanGate::AND) { L *= r.lower; U *= r.upper; }
      else { L *= (1.0 - r.lower); U *= (1.0 - r.upper); }
    }
    res = (op == BooleanGate::AND) ? DTreeInterval{L, U}
                                   : DTreeInterval{1.0 - L, 1.0 - U};
  } else if(live.size() == 1) {
    res = genRefine(ctx, live[0], A, w);
  } else {
    if(w > 0.0) {
      DTreeInterval b = genBoundGroup(ctx, op, live, A);
      if(b.upper - b.lower <= w)
        return b; // approximate: do not memoise an early-stopped interval
    }
    const gate_t x = genPivot(ctx, live, A);
    const double px = ctx.c.getProb(x);
    A[x] = true;
    DTreeInterval r1 = genRefineGroup(ctx, op, live, A, w);
    A[x] = false;
    DTreeInterval r0 = genRefineGroup(ctx, op, live, A, w);
    A.erase(x);
    res = {px * r1.lower + (1.0 - px) * r0.lower,
           px * r1.upper + (1.0 - px) * r0.upper};
  }

  if(exact)
    ctx.exactMemo.emplace(key, res.lower); // exact: lower == upper
  return res;
}

} // namespace

DTreeInterval dtreeBoundsCircuit(const BooleanCircuit &c, gate_t root,
                                 double max_width)
{
  GenContext ctx{c, {}, {}};
  Assignment A;
  return genRefine(ctx, root, A, max_width);
}

} // namespace provsql
