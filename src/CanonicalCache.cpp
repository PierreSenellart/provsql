#include "CanonicalCache.h"

#include <cassert>

namespace provsql {

namespace {

constexpr size_t MAX_CANONICAL_CACHE_SIZE = 1536u << 20;
constexpr size_t MAX_CANONICAL_ENTRY_SIZE = MAX_CANONICAL_CACHE_SIZE / 4;

size_t estimateEntryBytes(const dDNNF &dd, const CanonicalCache::Entry &e)
{
  size_t wires = 0, uuid_bytes = 0;
  for(gate_t g{0}; g < dd.getNbGates(); ++g) {
    wires += dd.getWires(g).size();
    uuid_bytes += dd.getUUID(g).size();
  }
  return e.form.size()
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

double instantiateAndEvaluate(const CanonicalCache::Entry &e,
                              const std::vector<double> &leaf_probs)
{
  assert(leaf_probs.size() == e.leaf_gate.size());
  dDNNF dd = e.dd;
  for(size_t i = 0; i < leaf_probs.size(); ++i)
    if(e.leaf_present[i])
      dd.setProb(e.leaf_gate[i], leaf_probs[i]);
  return dd.probabilityEvaluation();
}

void tryCacheCompiled(
  CanonicalCache &cache, const CanonicalForm &key, dDNNF &dd,
  const std::function<std::string(const CanonicalLeaf &)> &leaf_uuid)
{
  CanonicalCache::Entry e;
  e.form = key.form;
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

  e.bytes = estimateEntryBytes(dd, e);
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

  auto isAbstract = [&](gate_t g) {
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

CanonicalCache &CanonicalCache::instance()
{
  static CanonicalCache cache;
  return cache;
}

const CanonicalCache::Entry *CanonicalCache::find(const CanonicalForm &key)
{
  auto it = map_.find(key.form);
  if(it == map_.end()) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  lru_.splice(lru_.begin(), lru_, it->second);
  return &*it->second;
}

bool CanonicalCache::admits(size_t bytes)
{
  if(bytes <= MAX_CANONICAL_ENTRY_SIZE)
    return true;
  ++stats_.rejections;
  return false;
}

bool CanonicalCache::insert(const CanonicalForm &key, Entry entry)
{
  auto it = map_.find(key.form);
  if(it != map_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    return false;
  }
  bytes_ += entry.bytes;
  lru_.push_front(std::move(entry));
  map_[key.form] = lru_.begin();
  ++stats_.inserts;
  while(bytes_ > MAX_CANONICAL_CACHE_SIZE && lru_.size() > 1) {
    bytes_ -= lru_.back().bytes;
    map_.erase(lru_.back().form);
    lru_.pop_back();
    ++stats_.evictions;
  }
  return true;
}

void CanonicalCache::reset()
{
  lru_.clear();
  map_.clear();
  bytes_ = 0;
  stats_ = CanonicalCacheStats{};
}

CanonicalCacheStats CanonicalCache::stats() const
{
  CanonicalCacheStats s = stats_;
  s.entries = lru_.size();
  s.bytes = bytes_;
  return s;
}

bool canonicalCacheEligible(const BooleanCircuit &c, gate_t root)
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

double cachedComponent(const BooleanCircuit &c, gate_t root,
                       const CanonicalCompileFn &compile,
                       bool *ran_compiler)
{
  CanonicalCache &cache = CanonicalCache::instance();
  const IslandKey key = computeIslandKey(c, root);

  std::unordered_map<gate_t, size_t, hash_gate_t> island_index;
  for(size_t i = 0; i < key.island_roots.size(); ++i)
    island_index[key.island_roots[i]] = i;

  std::unordered_map<gate_t, double, hash_gate_t> island_prob;
  for(size_t i = 0; i < key.islands.size(); ++i) {
    const CanonicalForm &icf = key.islands[i];
    const gate_t g = key.island_roots[i];
    double p;
    if(const auto *e = cache.find(icf)) {
      std::vector<double> probs;
      probs.reserve(icf.leaves.size());
      for(const CanonicalLeaf &leaf : icf.leaves)
        probs.push_back(c.getProb(leaf.gate));
      p = instantiateAndEvaluate(*e, probs);
    } else {
      if(ran_compiler)
        *ran_compiler = true;
      BooleanCircuit island;
      gate_t island_root = materialiseComponent(c, g, nullptr, {}, {}, island);
      dDNNF dd = compile(island, island_root);
      tryCacheCompiled(cache, icf, dd, [&](const CanonicalLeaf &leaf) {
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

  if(const auto *e = cache.find(rcf))
    return instantiateAndEvaluate(*e, probs);

  if(ran_compiler)
    *ran_compiler = true;

  auto residualLeafUuid = [&](const CanonicalLeaf &leaf) {
    return leaf.is_abstract ? islandLeafUuid(island_index.at(leaf.gate))
                            : c.getUUID(leaf.gate);
  };

  if(key.island_roots.empty()) {
    dDNNF dd = compile(c, root);
    tryCacheCompiled(cache, rcf, dd, residualLeafUuid);
    return dd.probabilityEvaluation();
  }

  GateSet frontier(key.island_roots.begin(), key.island_roots.end());
  BooleanCircuit res;
  gate_t res_root = materialiseComponent(c, root, &frontier, island_index,
                                         island_prob, res);
  dDNNF dd = compile(res, res_root);
  tryCacheCompiled(cache, rcf, dd, residualLeafUuid);
  return dd.probabilityEvaluation();
}

}

double canonicalCachedProbability(const BooleanCircuit &c, gate_t root,
                                  const CanonicalCompileFn &compile,
                                  bool *ran_compiler)
{
  if(ran_compiler)
    *ran_compiler = false;
  return cachedComponent(c, root, compile, ran_compiler);
}

}
