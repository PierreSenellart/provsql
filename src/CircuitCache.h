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

struct CircuitCacheInfos
{
  pg_uuid_t token;
  gate_type type;
  std::vector<pg_uuid_t> children;

  inline unsigned size() const {
    return sizeof(CircuitCacheInfos)+children.size()*sizeof(pg_uuid_t);
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

bool insert(const CircuitCacheInfos& infos);
std::optional<CircuitCacheInfos> get(pg_uuid_t token) const;

inline iterator begin(){
  return il.begin();
}
inline iterator end(){
  return il.end();
}
};

#endif /* CIRCUIT_CACHE_CPP_H */
