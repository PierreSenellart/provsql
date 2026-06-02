/**
 * @file DTree.cpp
 * @brief Implementation of the d-tree anytime interval-bounds engine.
 */
#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "provsql_utils.h" // provsql_interrupted
#include "miscadmin.h"     // check_stack_depth
}

#include "BooleanCircuit.h"
#include "DTree.h"

namespace provsql {

namespace {

using Clauses = std::vector<std::set<gate_t> >;

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
 * must not be cached.
 */
struct DTreeContext {
  const BooleanCircuit &c;
  std::map<Clauses, double> memo;
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

} // namespace provsql
