/**
 * @file CircuitCache.h
 * @brief LRU in-process cache for recently created provenance circuit gates.
 *
 * Reading gates from the mmap-backed persistent storage is relatively
 * expensive.  The @c CircuitCache stores the most recently created gates
 * in an in-memory Boost multi-index container so that subsequent lookups
 * within the same backend can skip the IPC round-trip to the background
 * worker.
 *
 * The cache is bounded by a fixed byte budget.  When inserting a new
 * gate would exceed the budget the oldest entry (FIFO order) is
 * evicted.  Eviction simply drops the entry: the cache is a read
 * accelerator on top of a write-through design (every @c create_gate
 * goes to the worker via IPC regardless of cache state, see
 * @c provsql_mmap.c), so the worker already holds every gate the cache
 * ever did and no flush hook is required.
 *
 * The C-linkage wrapper functions in @c circuit_cache.h provide the
 * interface used by the C portions of the extension.
 */
#ifndef CIRCUIT_CACHE_CPP_H
#define CIRCUIT_CACHE_CPP_H

#include "provsql_utils_cpp.h"

#include <optional>
#include <utility>
#include <vector>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>

using namespace boost::multi_index;

/**
 * @brief All information stored for a single gate in the circuit cache.
 *
 * Each cache entry tracks the gate's UUID token, its type, and the list
 * of its children (also as UUIDs).
 */
struct CircuitCacheInfos
{
  pg_uuid_t token;               ///< UUID identifying this gate
  gate_type type;                ///< Kind of gate (input, plus, times, …)
  std::vector<pg_uuid_t> children; ///< Ordered list of child gate UUIDs

  /**
   * @brief Estimated memory footprint of this entry in bytes.
   *
   * Used by @c CircuitCache to track the total size of the cache so that
   * it can enforce its byte budget.
   *
   * @return Size of @c CircuitCacheInfos plus the children array.
   */
  inline unsigned size() const {
    return sizeof(CircuitCacheInfos)+children.size()*sizeof(pg_uuid_t);
  }
};

/**
 * @brief Bounded LRU cache mapping gate UUIDs to their @c CircuitCacheInfos.
 *
 * Internally a Boost @c multi_index_container provides two views of the
 * same data:
 * - A sequenced (insertion-order) index used to evict the oldest entry
 *   when the cache is full.
 * - A hashed-unique index keyed on @c token for O(1) lookup.
 *
 * The cache is not thread-safe; each backend process maintains its own
 * instance.
 */
class CircuitCache
{
/** @brief Boost multi-index container type for cache entries. */
typedef multi_index_container<
    CircuitCacheInfos,
    indexed_by<
      sequenced<>,
      hashed_unique<member<CircuitCacheInfos, pg_uuid_t, &CircuitCacheInfos::token> >
      >
    > item_list;
item_list il;           ///< The container holding cached entries
unsigned current_size;  ///< Current total byte usage of cached entries

public:
/** @brief The value type stored in the cache. */
typedef CircuitCacheInfos item_type;
/** @brief Iterator type for iterating over cache entries in FIFO order. */
typedef typename item_list::iterator iterator;

/** @brief Construct an empty cache. */
CircuitCache() : current_size(0) {
}

/**
 * @brief Insert a new gate into the cache, evicting the oldest if necessary.
 *
 * If @p infos.token is already present in the cache the existing entry
 * is bumped to the LRU front and, when @p infos carries strictly more
 * information than the existing entry (a real @c gate_type replacing a
 * stored @c gate_invalid placeholder, or a non-empty children list
 * replacing an empty one), its contents are overwritten in place.
 * Otherwise the entry is added and, if the cache then exceeds its size
 * budget, the oldest entry (FIFO tail) is removed.
 *
 * @param infos  Gate information to cache.
 * @return @c true if the entry was newly inserted, @c false if it was
 *         already present (regardless of whether it was upgraded).
 */
bool insert(const CircuitCacheInfos& infos);

/**
 * @brief Look up a gate by UUID.
 *
 * @param token  UUID of the gate to find.
 * @return An @c std::optional containing the @c CircuitCacheInfos if
 *         found, or @c std::nullopt on a cache miss.
 */
std::optional<CircuitCacheInfos> get(pg_uuid_t token) const;

/**
 * @brief Iterator to the first cached entry (oldest).
 * @return Iterator pointing to the oldest cache entry.
 */
inline iterator begin(){
  return il.begin();
}
/**
 * @brief Past-the-end iterator for the cache.
 * @return Iterator one past the last cache entry.
 */
inline iterator end(){
  return il.end();
}
};

#endif /* CIRCUIT_CACHE_CPP_H */
