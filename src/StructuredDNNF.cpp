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

#include <climits>
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
