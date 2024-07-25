#include <src/CircuitCache.h>

constexpr unsigned MAX_CIRCUIT_CACHE_SIZE = 1 << 20;

void CircuitCache::insert(const CircuitCacheInfos& infos)
{
  std::pair<iterator,bool> p=il.push_front(infos);

  if(!p.second) {
    il.relocate(il.begin(),p.first);
  } else {
    current_size+=infos.size();
  }
  while(current_size>MAX_CIRCUIT_CACHE_SIZE) {
    current_size -= (*il.end()).size();
    il.pop_back();
  }
}

CircuitCacheInfos CircuitCache::get(pg_uuid_t token) const
{
  auto it = il.get<1>().find(token);
  if(it!=il.get<1>().end())
    return *it;
  else
    return {};
}
