#ifndef CANONICAL_CACHE_H
#define CANONICAL_CACHE_H

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "CircuitCanonicalForm.h"
#include "dDNNF.h"

namespace provsql {

struct CanonicalCacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t evictions = 0;
  uint64_t rejections = 0;
  size_t entries = 0;
  size_t bytes = 0;
};

class CanonicalCache {
public:
struct Entry {
  std::string form;
  dDNNF dd;
  std::vector<gate_t> leaf_gate;
  std::vector<char> leaf_present;
  size_t bytes = 0;
};

static CanonicalCache &instance();

const Entry *find(const CanonicalForm &key);

bool admits(size_t bytes);

bool insert(const CanonicalForm &key, Entry entry);

void reset();

CanonicalCacheStats stats() const;

private:
CanonicalCache() = default;

std::list<Entry> lru_;
std::unordered_map<std::string, std::list<Entry>::iterator> map_;
size_t bytes_ = 0;
mutable CanonicalCacheStats stats_;
};

using CanonicalCompileFn =
  std::function<dDNNF(const BooleanCircuit &, gate_t)>;

bool canonicalCacheEligible(const BooleanCircuit &c, gate_t root);

double canonicalCachedProbability(const BooleanCircuit &c, gate_t root,
                                  const CanonicalCompileFn &compile,
                                  bool *ran_compiler = nullptr);

}

#endif
