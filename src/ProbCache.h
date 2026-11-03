#ifndef PROB_CACHE_H
#define PROB_CACHE_H

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "CircuitCanonicalForm.h"
#include "dDNNF.h"

namespace provsql {

struct ProbCacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t inserts = 0;
  uint64_t evictions = 0;
  uint64_t rejections = 0;
  size_t entries = 0;
  size_t bytes = 0;
};

class ProbCache {
public:
struct Entry {
  std::string form;
  dDNNF dd;
  std::vector<gate_t> leaf_gate;
  std::vector<char> leaf_present;
  size_t bytes = 0;
};

static ProbCache &instance();

const Entry *find(const CanonicalForm &key);

bool admits(size_t bytes);

bool insert(const CanonicalForm &key, Entry entry);

void reset();

ProbCacheStats stats() const;

private:
ProbCache() = default;

std::list<Entry> lru_;
std::unordered_map<std::string, std::list<Entry>::iterator> map_;
size_t bytes_ = 0;
mutable ProbCacheStats stats_;
};

using ProbCacheCompileFn =
  std::function<dDNNF(const BooleanCircuit &, gate_t)>;

struct ProbCacheOptions {
  bool use_cache = true;
  bool fold = false;
};

bool probCacheEligible(const BooleanCircuit &c, gate_t root);

double probCachedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbCacheCompileFn &compile,
                             bool *ran_compiler = nullptr,
                             const ProbCacheOptions &options = ProbCacheOptions());

}

#endif
