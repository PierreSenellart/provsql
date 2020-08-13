// Heavily inspired from https://stackoverflow.com/a/30938947, credits to
// Yakk - Adam Nevraum @ StackOverflow

#include <vector>
#include <utility>

template<class Key, class Value, template<class...>class Storage=std::vector>
struct flat_map {
  using storage_t = Storage<std::pair<Key, Value>>;
  storage_t storage;

  using iterator=decltype(begin(std::declval<storage_t&>()));
  using const_iterator=decltype(begin(std::declval<const storage_t&>()));

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
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>>>
  Value& operator[](K&& k){
    auto it = find(k);
    if (it != end()) return it->v;
    storage.emplace_back( std::forward<K>(k), Value{} );
    return storage.back().v;
  }
private:
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>>>
  auto key_match( K& k ) {
    return [&k](kv const& kv){
      return kv.k == k;
    };
  }
public:
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>>>
  iterator find(K&& k) {
    return std::find_if( begin(), end(), key_match(k) );
  }
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>>>
  const_iterator find(K&& k) const {
    return const_cast<flat_map*>(this)->find(k);
  }
  iterator erase(const_iterator it) {
    return storage.erase(it);
  }
};
