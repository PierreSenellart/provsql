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

DTreeInterval recurse(const BooleanCircuit &c, Clauses clauses, double max_width)
{
  check_stack_depth();
  if(provsql_interrupted)
    throw CircuitException("Interrupted");

  if(clauses.empty())
    return {0., 0.}; // empty disjunction is false
  for(const auto &cl : clauses)
    if(cl.empty())
      return {1., 1.}; // a clause with no literals is true, so the DNF is true

  removeSubsumed(clauses);

  // Cheap certified interval; stop as soon as it is narrow enough.
  double L, U;
  c.dnfBounds(clauses, L, U);
  if(U - L <= max_width)
    return {L, U};

  // Independent-or: recurse on each component with a 1/k share of the budget
  // (the OR width is at most the sum of component widths) and combine.
  std::vector<Clauses> comps = components(clauses);
  if(comps.size() > 1) {
    double prod_lower = 1., prod_upper = 1.;
    const double sub_width = max_width / static_cast<double>(comps.size());
    for(auto &comp : comps) {
      DTreeInterval r = recurse(c, std::move(comp), sub_width);
      prod_lower *= (1. - r.lower);
      prod_upper *= (1. - r.upper);
    }
    return {1. - prod_lower, 1. - prod_upper};
  }

  // Shannon expansion on the most frequent variable x: Pr = Pr[x]·Pr[Phi|x=1] +
  // (1-Pr[x])·Pr[Phi|x=0].  For a monotone DNF the cofactors are, on x=1, every
  // clause with x stripped of x (the literal is satisfied) and every clause
  // without x unchanged; on x=0, only the clauses not containing x survive.  The
  // same width budget passes to both branches (the mixture width is at most the
  // larger branch's).
  const gate_t x = mostFrequentVar(clauses);
  const double px = c.getProb(x);
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
  const DTreeInterval rp = recurse(c, std::move(pos), max_width);
  const DTreeInterval rn = recurse(c, std::move(neg), max_width);
  return {px * rp.lower + (1. - px) * rn.lower,
          px * rp.upper + (1. - px) * rn.upper};
}

} // namespace

DTreeInterval dtreeBounds(const BooleanCircuit &c, Clauses clauses,
                          double max_width)
{
  return recurse(c, std::move(clauses), max_width);
}

} // namespace provsql
