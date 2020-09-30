#ifndef FLAT_SET_H
#define FLAT_SET_H

// Heavily inspired from https://stackoverflow.com/a/30938947, credits to
// Yakk - Adam Nevraum @ StackOverflow

#include <vector>
#include <utility>
#include <algorithm>

template<class T, template<class...>class Storage=std::vector>
struct flat_set {
  using storage_t = Storage<T>;
  storage_t storage;

  using iterator=decltype(std::begin(std::declval<storage_t&>()));
  using const_iterator=decltype(std::begin(std::declval<const storage_t&>()));

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
public:
  template<class K, class=std::enable_if_t<std::is_convertible<K, T>{}>>
  iterator find(K&& k) {
    return std::find( begin(), end(), k );
  }
  template<class K, class=std::enable_if_t<std::is_convertible<K, T>{}>>
  const_iterator find(K&& k) const {
    return const_cast<flat_set*>(this)->find(k);
  }
  iterator erase(const_iterator it) {
    return storage.erase(it);
  }
  template<class K, class=std::enable_if_t<std::is_convertible<K,T>{}>>
  void insert( const K& value )
  {
    auto it = find(value);
    if (it == end())
      storage.emplace_back( value );
  }
  template<class K, class=std::enable_if_t<std::is_convertible<K,T>{}>>
  void insert( K&& value )
  {
    auto it = find(value);
    if (it == end())
      storage.emplace_back( std::forward<T>(value) );
  }

  bool operator==(const flat_set &rhs) const
  {
    if(size()!=rhs.size())
      return false;
    for(auto i: rhs)
      if(find(i)==end())
        return false;

    return true;
  }

  bool operator<(const flat_set &rhs) const
  {
    if(size()<rhs.size())
      return true;
    else if(size()>rhs.size())
      return false;
    T min{}; bool found=false;
    for(const auto &i: *this) {
      if(rhs.find(i)!=rhs.end())
        continue;
      if(found) {
        if(i<min)
          min=i;
      } else {
        min=i;
        found=true;
      }
    }
    if(!found)
      return false;
    T min2{}; found=false;
    for(const auto &i: rhs) {
      if(find(i)!=end())
        continue;
      if(found) {
        if(i<min2)
          min2=i;
      } else {
        min2=i;
        found=true;
      }
    }
    return min<min2;
  }
};

namespace std {
  template<class T, template<class...> class Storage>
  struct hash<flat_set<T, Storage>> {
    size_t operator()(const flat_set<T, Storage> &key) const
    {
      size_t result = 0xDEADBEEF;
      for(const auto &i: key)
        result ^= std::hash<T>()(i);
      return result;
    }
  };
}
#endif /* FLAT_SET_H */
