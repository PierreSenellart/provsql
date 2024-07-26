#include <src/CircuitCache.h>

extern "C" {
#include "circuit_cache.h"
}

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
