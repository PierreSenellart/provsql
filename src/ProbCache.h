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
  /// Method that compiled @c dd ("tree-decomposition", "compilation:d4"...).
  std::string builder;
  dDNNF dd;
  std::vector<gate_t> leaf_gate;
  std::vector<char> leaf_present;
  size_t bytes = 0;
};

static ProbCache &instance();

/// Looks @p key up.  A non-empty @p builder restricts the hit to an entry
/// compiled by that method; an entry of another builder counts as a miss.
const Entry *find(const CanonicalForm &key,
                  const std::string &builder = std::string());

bool admits(size_t bytes);

/// Stores @p entry under @p key, replacing an entry already there.
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

/// What a call to probCachedProbability actually did, so the caller can
/// report it instead of the method it was asked for.
struct ProbCacheReport {
  bool folded = false;               ///< root split into independent groups
  std::vector<std::string> built;    ///< builders run on a miss
  std::vector<std::string> cached;   ///< builders of the entries served

  /// Comma-separated label: "fold" if folded, the builders that ran, then
  /// "cache:<builder>" for each builder whose entry was served.
  std::string describe() const;
};

struct ProbCacheOptions {
  bool use_cache = true;
  bool fold = false;
  /// Non-empty: only an entry compiled by this builder is a hit (a call
  /// naming its method must not be answered from another method's entry).
  std::string required_builder;
  /// Names the builder of the d-DNNF the compile callback last returned.
  std::function<std::string()> last_builder;
  ProbCacheReport *report = nullptr;
};

struct ProbFoldEval {std::function<double(BooleanCircuit &, gate_t, bool)> part;
  std::function<double()> whole;
};

bool probCacheEligible(const BooleanCircuit &c, gate_t root);

double probFoldedProbability(const BooleanCircuit &c, gate_t root,const ProbFoldEval &eval);

double probCachedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbCacheCompileFn &compile,
                             bool *ran_compiler = nullptr,
                             const ProbCacheOptions &options = ProbCacheOptions());

}

#endif
