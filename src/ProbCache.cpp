/**
 * @file ProbCache.cpp
 * @brief Cache of compiled d-DNNFs and folding of independent parts (implementation).
 */
#include "ProbCache.h"

extern "C" {
#include "provsql_utils.h"
}

#include <algorithm>
#include <cassert>
#include <numeric>

namespace provsql {

namespace {

/// Budget of the cache, in bytes: @c provsql.prob_cache_size, and no more
/// than a quarter of @c provsql.max_memory when that is set.  What a
/// statement adds to the cache counts in the memory its evaluations take,
/// and the cache must not be what makes them pass the limit.
size_t maxCacheBytes()
{
  size_t budget = static_cast<size_t>(provsql_prob_cache_size) * 1024;
  if(provsql_max_memory > 0)
    budget = std::min(budget,
                      static_cast<size_t>(provsql_max_memory) * 1024 * 1024 / 4);
  return budget;
}

/// Largest entry worth keeping: beyond a quarter of the budget, storing
/// one d-DNNF would push out most of the others.
size_t maxEntryBytes()
{
  return maxCacheBytes() / 4;
}

constexpr unsigned MAX_FOLD_DEPTH = 64;

size_t estimateEntryBytes(const dDNNF &dd, const ProbCache::Entry &e)
{
  size_t wires = 0, uuid_bytes = 0;
  for(gate_t g{0}; g < dd.getNbGates(); ++g) {
    wires += dd.getWires(g).size();
    uuid_bytes += dd.getUUID(g).size();
  }
  // The form is held twice, as the key of the map and in the entry.
  return 2 * e.form.size()
         + dd.getNbGates() * 96
         + wires * sizeof(gate_t)
         + uuid_bytes * 2
         + e.leaf_gate.size() * (sizeof(gate_t) + 1)
         + 256;
}

std::string islandLeafUuid(size_t i)
{
  return "provsql:island:" + std::to_string(i);
}

double instantiateAndEvaluate(const ProbCache::Entry &e,
                              const std::vector<double> &leaf_probs)
{
  assert(leaf_probs.size() == e.leaf_gate.size());
  dDNNF dd = e.dd;
  for(size_t i = 0; i < leaf_probs.size(); ++i)
    if(e.leaf_present[i])
      dd.setProb(e.leaf_gate[i], leaf_probs[i]);
  return dd.probabilityEvaluation();
}

void noteOnce(std::vector<std::string> &names, const std::string &name)
{
  if(std::find(names.begin(), names.end(), name) == names.end())
    names.push_back(name);
}

void tryCacheCompiled(
  ProbCache &cache, const CanonicalForm &key, dDNNF &dd,
  const std::string &builder,
  const std::function<std::string(const CanonicalLeaf &)> &leaf_uuid)
{
  ProbCache::Entry e;
  e.form = key.form;
  e.builder = builder;
  e.leaf_gate.assign(key.leaves.size(), gate_t{0});
  e.leaf_present.assign(key.leaves.size(), 0);

  GateSet mapped;
  for(size_t i = 0; i < key.leaves.size(); ++i) {
    const std::string u = leaf_uuid(key.leaves[i]);
    if(u.empty())
        return;
    if(!dd.hasGate(u))
        continue;
    gate_t g = dd.getGate(u);
    if(dd.getGateType(g) != BooleanGate::IN)
      return;
    e.leaf_gate[i] = g;
    e.leaf_present[i] = 1;
    mapped.insert(g);
  }

  if(dd.getNbGates() > 0) {
    std::vector<gate_t> stack{dd.getRoot()};
    GateSet seen{dd.getRoot()};
    while(!stack.empty()) {
      gate_t g = stack.back();
      stack.pop_back();
      if(dd.getGateType(g) == BooleanGate::IN && !mapped.count(g))
        return;
      for(gate_t ch : dd.getWires(g))
        if(seen.insert(ch).second)
          stack.push_back(ch);
    }
  }

  e.bytes=estimateEntryBytes(dd, e);
  if(!cache.admits(e.bytes))
    return;
  e.dd = dd;
  cache.insert(key, std::move(e));
}

gate_t materialiseComponent(
  const BooleanCircuit &c, gate_t root, const GateSet *frontier,
  const std::unordered_map<gate_t, size_t, hash_gate_t> &island_index,
  const std::unordered_map<gate_t, double, hash_gate_t> &island_prob,
  BooleanCircuit &out)
{
  GateSet nodes = reachableNodes(c, root, frontier);
  std::unordered_map<gate_t, gate_t, hash_gate_t> old2new;
  old2new.reserve(nodes.size());

  auto isAbstract =[&](gate_t g) {
    return g != root && frontier && frontier->count(g) != 0;
  };

  for(gate_t g : nodes) {
    if(isAbstract(g))
      old2new[g] = out.setGate(islandLeafUuid(island_index.at(g)),
                               BooleanGate::IN, island_prob.at(g));
    else if(c.getGateType(g) == BooleanGate::IN)
      old2new[g] = out.setGate(c.getUUID(g), BooleanGate::IN, c.getProb(g));
    else
      old2new[g] = out.setGate(c.getGateType(g));
  }
  for(gate_t g : nodes) {
    if(isAbstract(g) || c.getGateType(g) == BooleanGate::IN)
      continue;
    for(gate_t u : c.getWires(g))
      out.addWire(old2new.at(g), old2new.at(u));
  }
  return old2new.at(root);
}

}

ProbCache &ProbCache::instance()
{
  static ProbCache cache;
  return cache;
}

const ProbCache::Entry *ProbCache::find(const CanonicalForm &key,
                                        const std::string &builder)
{
  auto it = map_.find(key.form);
  if(it == map_.end()
     || (!builder.empty() && it->second->builder != builder)) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  lru_.splice(lru_.begin(), lru_, it->second);
  return &*it->second;
}

bool ProbCache::admits(size_t bytes)
{
  if(bytes <= maxEntryBytes())
    return true;
  ++stats_.rejections;
  return false;
}

bool ProbCache::insert(const CanonicalForm &key, Entry entry)
{
  auto it = map_.find(key.form);
  if(it != map_.end()) {
    bytes_ -= it->second->bytes;
    lru_.erase(it->second);
    map_.erase(it);
  }
  bytes_ += entry.bytes;
  lru_.push_front(std::move(entry));
  map_[key.form] = lru_.begin();
  ++stats_.inserts;
  trim();
  return true;
}

void ProbCache::trim()
{
  while(bytes_ > maxCacheBytes() && !lru_.empty()) {
    bytes_ -= lru_.back().bytes;
    map_.erase(lru_.back().form);
    lru_.pop_back();
    ++stats_.evictions;
  }
}

void ProbCache::reset()
{
  lru_.clear();
  map_.clear();
  bytes_ = 0;
  stats_ = ProbCacheStats{};
}

ProbCacheStats ProbCache::stats() const
{
  ProbCacheStats s = stats_;
  s.entries = lru_.size();
  s.bytes = bytes_;
  s.budget = maxCacheBytes();
  return s;
}

bool probCacheEligible(const BooleanCircuit &c, gate_t root)
{
  for(gate_t g : reachableNodes(c, root)) {
    switch(c.getGateType(g)) {
    case BooleanGate::AND:
    case BooleanGate::OR:
    case BooleanGate::NOT:
      break;
    case BooleanGate::IN:
      if(c.getUUID(g).empty())
        return false;
      break;
    default:
      return false;
    }
  }
  return true;
}

namespace {

/// Runs @p compile, recording the builder it used in the report.
dDNNF compileNoted(const BooleanCircuit &c, gate_t root,
                   const ProbCacheCompileFn &compile, bool *ran_compiler,
                   const ProbCacheOptions &options, std::string &builder)
{
  if(ran_compiler)
    *ran_compiler = true;
  dDNNF dd = compile(c, root);
  builder = options.last_builder ? options.last_builder() : std::string();
  if(options.report)
    noteOnce(options.report->built, builder);
  return dd;
}

double cachedComponent(const BooleanCircuit &c, gate_t root,
                       const ProbCacheCompileFn &compile,
                       bool *ran_compiler, const ProbCacheOptions &options)
{
  ProbCache &cache = ProbCache::instance();
  cache.trim();
  const auto lookup = [&](const CanonicalForm &cf) {
    const ProbCache::Entry *e = cache.find(cf, options.required_builder);
    if(e && options.report)
      noteOnce(options.report->cached, e->builder);
    return e;
  };
  std::string builder;
  const IslandKey key = computeIslandKey(c, root);

  std::unordered_map<gate_t, size_t, hash_gate_t> island_index;
  for(size_t i = 0; i < key.island_roots.size(); ++i)
    island_index[key.island_roots[i]] = i;

  std::unordered_map<gate_t, double, hash_gate_t> island_prob;
  for(size_t i = 0; i < key.islands.size(); ++i) {
    const CanonicalForm &icf = key.islands[i];
    const gate_t g = key.island_roots[i];
    double p;
    if(const auto *e = lookup(icf)) {
      std::vector<double> probs;
      probs.reserve(icf.leaves.size());
      for(const CanonicalLeaf &leaf : icf.leaves)
        probs.push_back(c.getProb(leaf.gate));
      p = instantiateAndEvaluate(*e, probs);
    } else {
      BooleanCircuit island;
      gate_t island_root = materialiseComponent(c, g, nullptr, {}, {}, island);
      dDNNF dd = compileNoted(island, island_root, compile, ran_compiler,
                              options, builder);
      tryCacheCompiled(cache, icf, dd, builder, [&](const CanonicalLeaf &leaf) {
        return c.getUUID(leaf.gate);
      });
      p = dd.probabilityEvaluation();
    }
    island_prob[g] = p;
  }

  const CanonicalForm &rcf = key.residual;
  std::vector<double> probs;
  probs.reserve(rcf.leaves.size());
  for(const CanonicalLeaf &leaf : rcf.leaves)
    probs.push_back(leaf.is_abstract ? island_prob.at(leaf.gate)
                                     : c.getProb(leaf.gate));

  if(const auto *e = lookup(rcf))
    return instantiateAndEvaluate(*e, probs);

  auto residualLeafUuid = [&](const CanonicalLeaf &leaf) {
    return leaf.is_abstract ? islandLeafUuid(island_index.at(leaf.gate))
                            : c.getUUID(leaf.gate);
  };

  if(key.island_roots.empty()) {
    dDNNF dd = compileNoted(c, root, compile, ran_compiler, options, builder);
    tryCacheCompiled(cache, rcf, dd, builder, residualLeafUuid);
    return dd.probabilityEvaluation();
  }

  GateSet frontier(key.island_roots.begin(), key.island_roots.end());
  BooleanCircuit res;
  gate_t res_root = materialiseComponent(c, root, &frontier, island_index,
                                         island_prob, res);
  dDNNF dd = compileNoted(res, res_root, compile, ran_compiler, options,
                          builder);
  tryCacheCompiled(cache, rcf, dd, builder, residualLeafUuid);
  return dd.probabilityEvaluation();
}

double evaluateComponent(const BooleanCircuit &c, gate_t root,
                         const ProbCacheCompileFn &compile,
                         bool *ran_compiler, const ProbCacheOptions &options)
{
  if(options.use_cache)
    return cachedComponent(c, root, compile, ran_compiler, options);
  std::string builder;
  return compileNoted(c, root, compile, ran_compiler, options, builder)
         .probabilityEvaluation();
}

std::vector<std::vector<gate_t> > independentGroups(
  const BooleanCircuit &c, const std::vector<gate_t> &children)
{
  std::vector<size_t> parent(children.size());
  std::iota(parent.begin(), parent.end(), size_t{0});
  auto find = [&parent](size_t x) {
    while(parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  std::unordered_map<gate_t, size_t, CanonicalGateHash> owner;
  std::vector<gate_t> stack;
  for(size_t i = 0; i < children.size(); ++i) {
    stack.push_back(children[i]);
    while(!stack.empty()) {
      const gate_t g = stack.back();
      stack.pop_back();
      const auto &wires = c.getWires(g);
      if(wires.empty() && c.getGateType(g) != BooleanGate::IN)
        continue;
      const auto entry = owner.emplace(g, i);
      if(!entry.second) {
        parent[find(i)] = find(entry.first->second);
        continue;
      }
      stack.insert(stack.end(), wires.begin(), wires.end());
    }
  }

  std::unordered_map<size_t, size_t> position;
  std::vector<std::vector<gate_t> > groups;
  for(size_t i = 0; i < children.size(); ++i) {
    const auto entry = position.emplace(find(i), groups.size());
    if(entry.second)
      groups.emplace_back();
    groups[entry.first->second].push_back(children[i]);
  }
  return groups;
}

gate_t materialiseGroup(const BooleanCircuit &c, BooleanGate type,
                        const std::vector<gate_t> &children,
                        BooleanCircuit &out)
{
  std::unordered_map<gate_t, gate_t, CanonicalGateHash> old2new;
  std::vector<gate_t> nodes;
  std::vector<gate_t> stack(children.begin(), children.end());
  while(!stack.empty()) {
    const gate_t g = stack.back();
    stack.pop_back();
    if(!old2new.emplace(g, gate_t{0}).second)
      continue;
    nodes.push_back(g);
    const auto &wires = c.getWires(g);
    stack.insert(stack.end(), wires.begin(), wires.end());
  }
  std::sort(nodes.begin(), nodes.end());

  for(gate_t g : nodes)
    old2new[g] = c.getGateType(g) == BooleanGate::IN
                 ? out.setGate(c.getUUID(g), BooleanGate::IN, c.getProb(g))
                 : out.setGate(c.getGateType(g));
  for(gate_t g : nodes)
    if(c.getGateType(g) != BooleanGate::IN)
      for(gate_t u : c.getWires(g))
        out.addWire(old2new.at(g), old2new.at(u));

  const gate_t root = out.setGate(type);
  for(gate_t child : children)
    out.addWire(root, old2new.at(child));
  return root;
}

double foldedProbability(const BooleanCircuit &c, gate_t g,
                         const ProbFoldEval &eval, unsigned depth)
{
  const BooleanGate type = c.getGateType(g);
  if(type == BooleanGate::IN)
    return c.getProb(g);

  if(depth < MAX_FOLD_DEPTH
     && (type == BooleanGate::AND || type == BooleanGate::OR)) {
    std::vector<gate_t> children;
    GateSet seen;
    for(gate_t w : c.getWires(g))
      if(seen.insert(w).second)
        children.push_back(w);

    const auto groups = independentGroups(c, children);
    if(groups.size() != 1 || groups.front().size() == 1) {
      double product = 1.;
      for(const auto &group : groups) {
        double p;
        if(group.size() == 1) {
          p = foldedProbability(c, group.front(), eval, depth + 1);
        } else {
          BooleanCircuit part;
          const gate_t part_root = materialiseGroup(c, type, group, part);
          p = eval.part(part, part_root, true);
        }
        product *= type == BooleanGate::AND ? p : 1. - p;
      }
      return type == BooleanGate::AND ? product : 1. - product;
    }
  }

  if(depth == 0)
    return eval.whole();

  BooleanCircuit part;
  const gate_t part_root = materialiseComponent(c, g, nullptr, {}, {}, part);
  return eval.part(part, part_root, false);
}

}

double probFoldedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbFoldEval &eval)
{
  return foldedProbability(c, root, eval, 0);
}

double probCachedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbCacheCompileFn &compile,
                             bool *ran_compiler,
                             const ProbCacheOptions &options)
{
  if(ran_compiler)
    *ran_compiler = false;
  if(options.fold) {
    ProbFoldEval eval;
    bool whole = false;
    eval.part = [&](BooleanCircuit &part, gate_t part_root, bool) {
      return evaluateComponent(part, part_root, compile, ran_compiler,
                               options);
    };
    eval.whole = [&]() {
      whole = true;
      return evaluateComponent(c, root, compile, ran_compiler, options);
    };
    const double p = foldedProbability(c, root, eval, 0);
    if(options.report)
      options.report->folded = !whole;
    return p;
  }
  return evaluateComponent(c, root, compile, ran_compiler, options);
}

std::string ProbCacheReport::describe() const
{
  std::string label;
  const auto add = [&label](const std::string &name) {
    if(!label.empty())
      label += ",";
    label += name;
  };
  if(folded)
    add("fold");
  for(const std::string &b : built)
    if(!b.empty())
      add(b);
  for(const std::string &b : cached)
    add(b.empty() ? "cache" : "cache:" + b);
  return label;
}

}
