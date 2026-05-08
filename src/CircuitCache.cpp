/**
 * @file CircuitCache.cpp
 * @brief LRU circuit-gate cache implementation and C-linkage wrappers.
 *
 * Implements @c CircuitCache::insert() and @c CircuitCache::get(), and
 * the three C-linkage wrapper functions declared in @c circuit_cache.h:
 * - @c circuit_cache_create_gate()
 * - @c circuit_cache_get_children()
 * - @c circuit_cache_get_type()
 *
 * The cache is a process-local Boost multi-index container bounded by
 * @c MAX_CIRCUIT_CACHE_SIZE bytes.  On overflow, the oldest (FIFO) entry
 * is evicted.  The C wrappers manage a singleton @c CircuitCache instance
 * and translate between @c pg_uuid_t / @c gate_type (C types) and the
 * C++ @c CircuitCacheInfos structure.
 */
#include <src/CircuitCache.h>

extern "C" {
#include "circuit_cache.h"
}

/** @brief Maximum total byte size of the in-process circuit gate cache (1 MiB). */
constexpr unsigned MAX_CIRCUIT_CACHE_SIZE = 1 << 20;

bool CircuitCache::insert(const CircuitCacheInfos& infos)
{
  std::pair<iterator,bool> p=il.push_front(infos);

  if(!p.second) {
    /* Key collision: an entry for this token already exists. If the
     * incoming entry carries more information (i.e. a real type
     * replacing a placeholder gate_invalid stored by get_children),
     * overwrite it; otherwise just touch the LRU position. The
     * eviction loop is not re-run here: a replace can only grow an
     * entry by (delta children count) * sizeof(pg_uuid_t), which is
     * negligible against MAX_CIRCUIT_CACHE_SIZE and self-corrects on
     * the next true insert. */
    auto current_size_delta = static_cast<long>(infos.size())
                              - static_cast<long>(p.first->size());
    bool replace = (p.first->type == gate_invalid && infos.type != gate_invalid)
                   || (p.first->children.empty() && !infos.children.empty());
    if(replace) {
      il.replace(p.first, infos);
      current_size = static_cast<unsigned>(
        static_cast<long>(current_size) + current_size_delta);
    }
    il.relocate(il.begin(),p.first);
    return false;
  } else {
    current_size+=infos.size();
    while(current_size>MAX_CIRCUIT_CACHE_SIZE && !il.empty()) {
      /* Evict the LRU tail. Use back() rather than *il.end() to avoid
       * dereferencing the past-the-end iterator (undefined behaviour). */
      current_size -= il.back().size();
      il.pop_back();
    }
    return true;
  }
}

std::optional<CircuitCacheInfos> CircuitCache::get(pg_uuid_t token) const
{
  auto it = il.get<1>().find(token);
  if(it!=il.get<1>().end())
    return *it;
  else
    return {};
}

/** @brief Process-local singleton circuit gate cache. */
static CircuitCache cache;

bool circuit_cache_create_gate(pg_uuid_t token, gate_type type, unsigned nb_children, pg_uuid_t *children)
{
  return cache.insert({token, type, std::vector<pg_uuid_t>(children, children+nb_children)});
}

unsigned circuit_cache_get_children(pg_uuid_t token, pg_uuid_t **children)
{
  auto opt = cache.get(token);

  if(opt) {
    auto nb_children = opt.value().children.size();
    /* Avoid calloc(0, ...): on glibc this returns a non-null pointer,
     * which would defeat the caller's `if(!children)` cache-miss
     * check. Treat zero-children cache entries as nullptr/0. */
    if(nb_children == 0) {
      *children = nullptr;
      return 0;
    }
    *children=reinterpret_cast<pg_uuid_t*>(calloc(nb_children, sizeof(pg_uuid_t)));
    for(unsigned i=0; i<nb_children; ++i)
      (*children)[i] = opt.value().children[i];
    return nb_children;
  } else {
    *children = nullptr;
    return 0;
  }
}

gate_type circuit_cache_get_type(pg_uuid_t token)
{
  auto opt = cache.get(token);
  if(opt)
    return opt.value().type;
  else
    return gate_invalid;
}
