#ifndef FLAT_MAP_H
#define FLAT_MAP_H

// Heavily inspired from https://stackoverflow.com/a/30938947, credits to
// Yakk - Adam Nevraum @ StackOverflow

#include <vector>
#include <utility>
#include <algorithm>

template<class Key, class Value, template<class...>class Storage=std::vector>
struct flat_map {
  using storage_t = Storage<std::pair<Key, Value>>;
  storage_t storage;

  using iterator=decltype(begin(std::declval<storage_t&>()));
  using const_iterator=decltype(begin(std::declval<const storage_t&>()));

  // Constructor
  flat_map() = default;
  flat_map(std::initializer_list<std::pair<Key,Value>> init) : storage(std::move(init)) { }

  // boilerplate:
  iterator begin() {
    using std::begin;
    return begin(storage);
  }
  const_iterator begin() const {
    using std::begin;
    return begin(storage);
  }
  const_iterator cbegin() const {
    using std::begin;
    return begin(storage);
  }
  iterator end() {
    using std::end;
    return end(storage);
  }
  const_iterator end() const {
    using std::end;
    return end(storage);
  }
  const_iterator cend() const {
    using std::end;
    return end(storage);
  }
  size_t size() const {
    return storage.size();
  }
  bool empty() const {
    return storage.empty();
  }
  // these only have to be valid if called:
  void reserve(size_t n) {
    storage.reserve(n);
  }
  size_t capacity() const {
    return storage.capacity();
  }
  // map-like interface:
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  Value& operator[](K&& k){
    auto it = find(k);
    if (it != end()) return it->second;
    storage.emplace_back( std::forward<K>(k), Value{} );
    return storage.back().second;
  }
private:
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  auto key_match( K& k ) {
    return [&k](const std::pair<Key, Value>& kv){
      return kv.first == k;
    };
  }
public:
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  iterator find(K&& k) {
    return std::find_if( begin(), end(), key_match(k) );
  }
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  const_iterator find(K&& k) const {
    return const_cast<flat_map*>(this)->find(k);
  }
  iterator erase(const_iterator it) {
    return storage.erase(it);
  }
  template<class P, class=std::enable_if_t<std::is_convertible<P,std::pair<Key, Value>>{}>>
  void insert( P&& value )
  {
    auto it = find(value.first);
    if (it == end())
      storage.emplace_back( std::forward<P>(value) );
    else
      it->second = value.second;
  }

  bool operator==(const flat_map &rhs) const
  {
    if(size()!=rhs.size())
      return false;
    for(const auto &[k, v]: rhs) {
      auto it = find(k);
      if(it==end())
        return false;
      else if(it->second != v)
        return false;
    }

    return true;
  }
  
  bool operator<(const flat_map &rhs) const
  {
    if(size()<rhs.size())
      return true;
    else if(size()>rhs.size())
      return false;
    storage_t x1{storage}, x2{rhs.storage};
    std::sort(x1.begin(), x1.end());
    std::sort(x2.begin(), x2.end());

    for(auto it1 = x1.begin(), it2 = x2.begin();
        it1!=x1.end();
        ++it1, ++it2) {
      if(*it1<*it2)
        return true;
      else if(*it1>*it2)
        return false;
    }

    return false;
  }
};

namespace std {
  template<class K, class V, template<class...> class Storage>
  struct hash<flat_map<K, V, Storage>> {
    size_t operator()(const flat_map<K, V, Storage> &key) const
    {
      size_t result = 0xDEADBEEF;
      for(const auto &[k,v]: key)
        result ^= std::hash<K>()(k) ^ std::hash<V>()(v);
      return result;
    }
  };
}
#endif /* FLAT_MAP_H */
