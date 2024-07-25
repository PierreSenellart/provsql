#ifndef CIRCUIT_CACHE_HPP
#define CIRCUIT_CACHE_HPP

#include "provsql_utils_cpp.h"

#include <utility>
#include <vector>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>

constexpr unsigned MAX_CIRCUIT_CACHE_SIZE = 1 << 20;

using namespace boost::multi_index;

struct CircuitCacheInfos
{
  pg_uuid_t token;
  unsigned nb_children;
  std::vector<pg_uuid_t> children;

  inline unsigned size() const {
    return sizeof(CircuitCacheInfos)+children.capacity()*sizeof(pg_uuid_t);
  }
};

class CircuitCache
{
typedef multi_index_container<
    CircuitCacheInfos,
    indexed_by<
      sequenced<>,
      hashed_unique<member<CircuitCacheInfos, pg_uuid_t, &CircuitCacheInfos::token> >
      >
    > item_list;
item_list il;
unsigned current_size;

public:
typedef CircuitCacheInfos item_type;
typedef typename item_list::iterator iterator;

CircuitCache() : current_size(0) {
}

void insert(const CircuitCacheInfos& infos)
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

inline iterator begin(){
  return il.begin();
}
inline iterator end(){
  return il.end();
}
};

#endif /* CIRCUIT_CACHE_HPP */
