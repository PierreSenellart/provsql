/**
 * @file CircuitCanonicalForm.cpp
 * @brief Canonical form of a Boolean circuit and its split into islands (implementation).
 */
#include "CircuitCanonicalForm.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace provsql {

namespace {

using idx_t = size_t;

unsigned typeCode(const BooleanCircuit &c, gate_t g, bool is_abstract)
{
  if(is_abstract)
    return 4;
  switch(c.getGateType(g)) {
  case BooleanGate::AND:   return 1;
  case BooleanGate::OR:    return 2;
  case BooleanGate::NOT:   return 3;
  case BooleanGate::IN:    return 4;
  case BooleanGate::MULIN: return 5;
  case BooleanGate::MULVAR: return 6;
  case BooleanGate::UNDETERMINED: return 0;
  }
  return 0;
}

const char *typeToken(const BooleanCircuit &c, gate_t g, bool is_abstract)
{
  if(is_abstract)
    return "V";
  switch(c.getGateType(g)) {
  case BooleanGate::AND:   return "AND";
  case BooleanGate::OR:    return "OR";
  case BooleanGate::NOT:   return "NOT";
  case BooleanGate::IN:    return "V";
  case BooleanGate::MULIN: return "MUL";
  case BooleanGate::MULVAR: return "MULVAR";
  case BooleanGate::UNDETERMINED: return "UNDET";
  }
  return "UNDET";
}

bool isLeafToken(const BooleanCircuit &c, gate_t g, bool is_abstract)
{
  return is_abstract || c.getGateType(g) == BooleanGate::IN;
}

}

GateSet reachableNodes(const BooleanCircuit &c, gate_t root,
                       const GateSet *abstract_leaves)
{
  GateSet nodes;
  std::vector<gate_t> stack{root};
  nodes.insert(root);
  while(!stack.empty()) {
    gate_t g = stack.back();
    stack.pop_back();
    if(g != root && abstract_leaves && abstract_leaves->count(g))
      continue;
    for(gate_t ch : c.getWires(g))
      if(nodes.insert(ch).second)
        stack.push_back(ch);
  }
  return nodes;
}

CanonicalForm computeCanonicalForm(const BooleanCircuit &c, gate_t root,
                                   const GateSet *abstract_leaves)
{
  auto isAbstract = [&](gate_t g) {
    return g != root && abstract_leaves && abstract_leaves->count(g) != 0;
  };
  static const std::vector<gate_t> no_children;
  auto childrenOf = [&](gate_t g) -> const std::vector<gate_t>& {
    return isAbstract(g) ? no_children : c.getWires(g);
  };

  GateSet node_set = reachableNodes(c, root, abstract_leaves);

  std::vector<gate_t> nodes(node_set.begin(), node_set.end());
  std::unordered_map<gate_t, idx_t, CanonicalGateHash> index;
  index.reserve(nodes.size());
  for(idx_t i = 0; i < nodes.size(); ++i)
    index[nodes[i]] = i;
  const idx_t n = nodes.size();

  std::vector<uint64_t> fanout(n, 0);
  std::vector<idx_t> pending(n, 0);
  for(idx_t i = 0; i < n; ++i) {
    const auto &ch = childrenOf(nodes[i]);
    pending[i] = ch.size();
    for(gate_t u : ch)
      ++fanout[index.at(u)];
  }

  std::vector<std::vector<idx_t> > consumers(n);
  for(idx_t i = 0; i < n; ++i)
    for(gate_t u : childrenOf(nodes[i]))
      consumers[index.at(u)].push_back(i);

  std::vector<uint64_t> height(n, 0);
  std::vector<idx_t> worklist;
  for(idx_t i = 0; i < n; ++i)
    if(pending[i] == 0)
      worklist.push_back(i);
  size_t processed = 0;
  while(processed < worklist.size()) {
    idx_t v = worklist[processed++];
    for(idx_t w : consumers[v]) {
      if(height[w] < height[v] + 1)
        height[w] = height[v] + 1;
      if(--pending[w] == 0)
        worklist.push_back(w);
    }
  }
  assert(worklist.size() == n);

  uint64_t max_height = 0;
  for(idx_t i = 0; i < n; ++i)
    max_height = std::max(max_height, height[i]);
  std::vector<std::vector<idx_t> > by_height(max_height + 1);
  for(idx_t i = 0; i < n; ++i)
    by_height[height[i]].push_back(i);

  std::vector<uint64_t> col(n, 0);
  uint64_t colours_assigned = 0;
  std::vector<uint64_t> sig;
  for(uint64_t h = 0; h <= max_height; ++h) {
    std::vector<std::pair<std::vector<uint64_t>, idx_t> > sigs;
    sigs.reserve(by_height[h].size());
    for(idx_t v : by_height[h]) {
      gate_t g = nodes[v];
      const bool abs = isAbstract(g);
      sig.clear();
      sig.push_back(typeCode(c, g, abs));
      sig.push_back(fanout[v]);
      const auto &ch = childrenOf(g);
      const size_t list_start = sig.size();
      for(gate_t u : ch)
        sig.push_back(col[index.at(u)]);
      const auto t = c.getGateType(g);
      if(!abs && (t == BooleanGate::AND || t == BooleanGate::OR))
        std::sort(sig.begin() + list_start, sig.end());
      sigs.emplace_back(sig, v);
    }
    std::sort(sigs.begin(), sigs.end());
    uint64_t rank = 0;
    for(size_t k = 0; k < sigs.size(); ++k) {
      if(k > 0 && sigs[k].first != sigs[k-1].first)
        ++rank;
      col[sigs[k].second] = colours_assigned + rank + 1;
    }
    if(!sigs.empty())
      colours_assigned += rank + 1;
  }

  std::vector<std::vector<gate_t> > canonical_children(n);
  for(idx_t i = 0; i < n; ++i) {
    canonical_children[i] = childrenOf(nodes[i]);
    const auto t = c.getGateType(nodes[i]);
    if(!isAbstract(nodes[i]) &&
       (t == BooleanGate::AND || t == BooleanGate::OR))
      std::stable_sort(canonical_children[i].begin(),
                       canonical_children[i].end(),
                       [&](gate_t a, gate_t b) {
        return col[index.at(a)] < col[index.at(b)];
      });
  }

  CanonicalForm cf;
  std::vector<uint64_t> number(n, 0);
  uint64_t counter = 0;

  struct Frame { idx_t v; size_t next_child; };
  std::vector<Frame> stack;
  std::string &out = cf.form;

  auto firstVisit = [&](idx_t v) {
    gate_t g = nodes[v];
    const bool abs = isAbstract(g);
    number[v] = ++counter;
    if(isLeafToken(c, g, abs)) {
      out += typeToken(c, g, abs);
      cf.leaves.push_back(CanonicalLeaf{g, abs});
    } else {
      out += '(';
      out += typeToken(c, g, abs);
      stack.push_back(Frame{v, 0});
    }
  };

  firstVisit(index.at(root));
  while(!stack.empty()) {
    Frame &f = stack.back();
    const auto &ch = canonical_children[f.v];
    if(f.next_child < ch.size()) {
      idx_t u = index.at(ch[f.next_child++]);
      out += ' ';
      if(number[u] != 0) {
        out += '#';
        out += std::to_string(number[u]);
      } else {
        firstVisit(u);
      }
    } else {
      out += ')';
      stack.pop_back();
    }
  }

  return cf;
}

std::vector<gate_t> maximalIslandRoots(const BooleanCircuit &c, gate_t root)
{
  GateSet node_set = reachableNodes(c, root);
  std::vector<gate_t> nodes(node_set.begin(), node_set.end());
  std::unordered_map<gate_t, idx_t, CanonicalGateHash> index;
  index.reserve(nodes.size());
  for(idx_t i = 0; i < nodes.size(); ++i)
    index[nodes[i]] = i;
  const idx_t n = nodes.size();
  const idx_t root_idx = index.at(root);

  if(n <= 1)
    return {};

  std::vector<std::vector<idx_t> > kids(n);
  for(idx_t i = 0; i < n; ++i) {
    const auto &wires = c.getWires(nodes[i]);
    kids[i].reserve(wires.size());
    for(gate_t u : wires)
      kids[i].push_back(index.at(u));
  }

  std::vector<uint64_t> start(n, 0), end(n, 0), last(n, 0);
  std::vector<bool> marked(n, false);
  std::vector<idx_t> finished;
  finished.reserve(n);
  uint64_t t = 0;

  struct Frame {
    idx_t v;
    size_t next_child;
  };
  std::vector<Frame> stack;

  auto visit = [&](idx_t x) {
    ++t;
    last[x] = t;
    if(!marked[x]) {
      marked[x] = true;
      start[x] = t;
      stack.push_back(Frame{x, 0});
    }
  };

  visit(root_idx);
  while(!stack.empty()) {
    const idx_t v = stack.back().v;
    if(stack.back().next_child < kids[v].size()) {
      const idx_t u = kids[v][stack.back().next_child++];
      visit(u);
    } else {
      stack.pop_back();
      if(!kids[v].empty()) {
        ++t;
        last[v] = t;
      }
      end[v] = t;
      finished.push_back(v);
    }
  }
  assert(finished.size() == n);

  const uint64_t NO_DATE = std::numeric_limits<uint64_t>::max();
  std::vector<uint64_t> lo(n, NO_DATE), hi(n, 0);
  for(idx_t v : finished)
    for(idx_t u : kids[v]) {
      lo[v] = std::min(lo[v], std::min(start[u], lo[u]));
      hi[v] = std::max(hi[v], std::max(last[u], hi[u]));
    }

  std::vector<bool> in_r(n, false);
  for(idx_t v = 0; v < n; ++v) {
    if(v == root_idx || kids[v].empty())
      continue;
    const auto type = c.getGateType(nodes[v]);
    if(type != BooleanGate::AND && type != BooleanGate::OR &&
       type != BooleanGate::NOT)
      continue;
    in_r[v] = start[v] < lo[v] && hi[v] < end[v];
  }

  std::vector<bool> reached(n, false);
  std::vector<idx_t> queue{root_idx};
  reached[root_idx] = true;
  std::vector<gate_t> result;
  for(size_t head = 0; head < queue.size(); ++head) {
    const idx_t x = queue[head];
    if(in_r[x]) {
      result.push_back(nodes[x]);
      continue;
    }
    for(idx_t u : kids[x])
      if(!reached[u]) {
        reached[u] = true;
        queue.push_back(u);
      }
  }
  return result;
}

IslandKey computeIslandKey(const BooleanCircuit &c, gate_t root)
{
  IslandKey key;
  std::vector<gate_t> frontier = maximalIslandRoots(c, root);
  if(frontier.empty()) {
    key.residual = computeCanonicalForm(c, root);
    return key;
  }

  GateSet fs(frontier.begin(), frontier.end());
  key.residual = computeCanonicalForm(c, root, &fs);

  for(const CanonicalLeaf &leaf : key.residual.leaves)
    if(leaf.is_abstract) {
      key.island_roots.push_back(leaf.gate);
      key.islands.push_back(computeCanonicalForm(c, leaf.gate));
    }
  assert(key.island_roots.size() == frontier.size());
  return key;
}

}
