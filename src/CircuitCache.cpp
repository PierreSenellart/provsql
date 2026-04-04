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
    il.relocate(il.begin(),p.first);
    return true;
  } else {
    current_size+=infos.size();
    while(current_size>MAX_CIRCUIT_CACHE_SIZE && !il.empty()) {
      current_size -= (*il.end()).size();
      il.pop_back();
    }
    return false;
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
