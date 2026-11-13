/**
 * @file ProbCache.h
 * @brief Cache of compiled d-DNNFs, and folding of independent parts.
 *
 * Two ways of not compiling the d-DNNF of a whole circuit, used by the
 * tree-decomposition and compilation probability methods:
 *
 * - the *cache* keeps, for the session, the d-DNNFs compiled so far,
 *   under the canonical form of the circuit they were compiled for
 *   (@c CircuitCanonicalForm.h).  A circuit is looked up island by
 *   island, then for its residual, so that a shape repeated inside a
 *   circuit or across circuits is compiled once; a hit evaluates the
 *   stored d-DNNF with the probabilities of the circuit at hand;
 * - the *fold* groups the children of an AND or OR root by the inputs
 *   they share.  Groups are independent, so their probabilities are
 *   multiplied (for an OR, through the complements); a group of one
 *   child is folded in turn and a larger one is handed to the method.
 *
 * Whether each applies is decided by the caller, from
 * @c provsql.prob_cache and @c provsql.idp_fold.
 */
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

/// Counters of the cache since it was last emptied.
struct ProbCacheStats {
  uint64_t hits = 0;        ///< Lookups served from an entry
  uint64_t misses = 0;      ///< Lookups that found no usable entry
  uint64_t inserts = 0;     ///< Entries stored
  uint64_t evictions = 0;   ///< Entries dropped to stay under the size limit
  uint64_t rejections = 0;  ///< d-DNNFs too large to be stored
  size_t entries = 0;       ///< Entries held now
  size_t bytes = 0;         ///< Estimated size of the entries held now
  size_t budget = 0;        ///< Size the entries may reach before some are dropped
};

/**
 * @brief The cache of compiled d-DNNFs of one backend.
 *
 * Entries are dropped least recently used first once their estimated
 * total size passes the budget, and a d-DNNF larger than a quarter of
 * the budget is not stored.  The budget is @c provsql.prob_cache_size,
 * lowered to a quarter of @c provsql.max_memory when that is smaller.
 */
class ProbCache {
public:
/// A compiled d-DNNF and how its inputs match the leaves of its form.
struct Entry {
  std::string form;                ///< Canonical form the d-DNNF was compiled for
  /// Method that compiled @c dd ("tree-decomposition", "compilation:d4"...).
  std::string builder;
  dDNNF dd;                        ///< The d-DNNF
  std::vector<gate_t> leaf_gate;   ///< Input of @c dd for each leaf of the form
  std::vector<char> leaf_present;  ///< Whether that leaf occurs in @c dd at all
  size_t bytes = 0;                ///< Estimated size of the entry
};

/// The cache of this backend.
static ProbCache &instance();

/// Looks @p key up.  A non-empty @p builder restricts the hit to an entry
/// compiled by that method; an entry of another builder counts as a miss.
const Entry *find(const CanonicalForm &key,
                  const std::string &builder = std::string());

/// Whether an entry of @p bytes is small enough to be stored; counts a
/// rejection otherwise.
bool admits(size_t bytes);

/// Stores @p entry under @p key, replacing an entry already there.
bool insert(const CanonicalForm &key, Entry entry);

/// Drops the least recently used entries until the cache fits its budget
/// (which the setting may have lowered since the last insertion).
void trim();

/// Empties the cache and its counters.
void reset();

/// The counters, with the current number and size of entries.
ProbCacheStats stats() const;

private:
ProbCache() = default;

std::list<Entry> lru_;
std::unordered_map<std::string, std::list<Entry>::iterator> map_;
size_t bytes_ = 0;
mutable ProbCacheStats stats_;
};

/// Compiles the circuit below a gate into a d-DNNF.
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

/// What probCachedProbability may do, and what it reports back.
struct ProbCacheOptions {
  bool use_cache = true;  ///< Look compiled d-DNNFs up, and store new ones
  bool fold = false;      ///< Fold the independent groups of the root first
  /// Non-empty: only an entry compiled by this builder is a hit (a call
  /// naming its method must not be answered from another method's entry).
  std::string required_builder;
  /// Names the builder of the d-DNNF the compile callback last returned.
  std::function<std::string()> last_builder;
  ProbCacheReport *report = nullptr;
};

/// How probFoldedProbability evaluates what it cannot fold.
struct ProbFoldEval {
  /// Probability of a part, given as a circuit of its own and its root;
  /// the flag tells a group of several children sharing inputs from a
  /// single subcircuit.
  std::function<double(BooleanCircuit &, gate_t, bool)> part;
  /// Probability of the whole circuit, when its root has nothing to fold.
  std::function<double()> whole;
};

/// Whether the circuit below @p root is one the cache and the fold handle:
/// AND, OR and NOT gates over inputs that carry a UUID.
bool probCacheEligible(const BooleanCircuit &c, gate_t root);

/// Probability of the circuit below @p root, folding the independent
/// groups of AND and OR gates from the root down and leaving the rest to
/// @p eval.
double probFoldedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbFoldEval &eval);

/**
 * @brief Probability of the circuit below @p root through the fold and
 *        the cache.
 * @param c            The circuit.
 * @param root         Its root.
 * @param compile      Compiles what is neither folded nor served.
 * @param ran_compiler If not null, set to whether @p compile was called.
 * @param options      What to use, and where to report what was done.
 */
double probCachedProbability(const BooleanCircuit &c, gate_t root,
                             const ProbCacheCompileFn &compile,
                             bool *ran_compiler = nullptr,
                             const ProbCacheOptions &options = ProbCacheOptions());

}

#endif
