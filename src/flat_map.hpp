/**
 * @file flat_map.hpp
 * @brief Flat (unsorted, contiguous-storage) associative map template.
 *
 * Heavily inspired from https://stackoverflow.com/a/30938947, credits to
 * Yakk - Adam Nevraum @ StackOverflow
 *
 * @c flat_map<Key, Value, Storage> is a lightweight associative container
 * that stores key-value pairs in a contiguous sequence (by default a
 * @c std::vector, but any compatible random-access container works).
 *
 * **Trade-offs vs. @c std::map / @c std::unordered_map**:
 * - Lookup and insertion are O(n) (linear scan), but for *small* maps (up
 *   to ~20 elements) cache-friendly linear search outperforms pointer-
 *   chasing tree or hash-table traversals.
 * - The storage type is a template parameter so that
 *   @c boost::container::static_vector can be used, enabling stack
 *   allocation and avoiding heap allocation entirely for small maps with
 *   bounded keys (e.g. keys bounded by the treewidth + 1 of a circuit).
 * - No iterator invalidation on insertions for @c static_vector.
 *
 * ProvSQL uses this as the @c valuation_t type in
 * @c dDNNFTreeDecompositionBuilder: a mapping from @c gate_t to @c bool
 * for the current bag's truth-value assignment.
 *
 * A @c std::hash specialisation is provided so that @c flat_map can be
 * used as a key in @c std::unordered_map.
 */
#ifndef FLAT_MAP_H
#define FLAT_MAP_H

// Heavily inspired from https://stackoverflow.com/a/30938947, credits to
// Yakk - Adam Nevraum @ StackOverflow

#include <vector>
#include <utility>
#include <algorithm>

/**
 * @brief Flat associative map with pluggable storage.
 *
 * @tparam Key      Key type.  Must be equality-comparable.
 * @tparam Value    Mapped value type.
 * @tparam Storage  Container template used for storage (default:
 *                  @c std::vector).  Must support @c emplace_back,
 *                  @c erase, @c begin, @c end, @c size, and @c empty.
 */
template<class Key, class Value, template<class...>class Storage=std::vector>
struct flat_map {
  using storage_t = Storage<std::pair<Key, Value>>; ///< Underlying container type
  storage_t storage; ///< Contiguous key-value storage

  using iterator=decltype(begin(std::declval<storage_t&>())); ///< Mutable iterator
  using const_iterator=decltype(begin(std::declval<const storage_t&>())); ///< Const iterator

  // Constructor
  flat_map() = default;
  /** @brief Construct from an initializer list of key-value pairs. @param init Initializer list. */
  flat_map(std::initializer_list<std::pair<Key,Value>> init) : storage(std::move(init)) { }

  // boilerplate:
  /** @brief Return iterator to the first element. @return Mutable begin iterator. */
  iterator begin() {
    using std::begin;
    return begin(storage);
  }
  /** @brief Return const iterator to the first element. @return Const begin iterator. */
  const_iterator begin() const {
    using std::begin;
    return begin(storage);
  }
  /** @brief Return const iterator to the first element. @return Const begin iterator. */
  const_iterator cbegin() const {
    using std::begin;
    return begin(storage);
  }
  /** @brief Return iterator past the last element. @return Mutable end iterator. */
  iterator end() {
    using std::end;
    return end(storage);
  }
  /** @brief Return const iterator past the last element. @return Const end iterator. */
  const_iterator end() const {
    using std::end;
    return end(storage);
  }
  /** @brief Return const iterator past the last element. @return Const end iterator. */
  const_iterator cend() const {
    using std::end;
    return end(storage);
  }
  /** @brief Return the number of key-value pairs. @return Number of elements. */
  size_t size() const {
    return storage.size();
  }
  /** @brief Return @c true if the map contains no elements. @return @c true if empty. */
  bool empty() const {
    return storage.empty();
  }
  // these only have to be valid if called:
  /** @brief Reserve storage for at least @p n elements. @param n Minimum capacity. */
  void reserve(size_t n) {
    storage.reserve(n);
  }
  /** @brief Return the current storage capacity. @return Number of elements that can be stored without reallocation. */
  size_t capacity() const {
    return storage.capacity();
  }
  // map-like interface:
  /**
   * @brief Access or insert the value for key @p k.
   * @param k  Key to look up or insert.
   * @return   Reference to the associated value.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  Value& operator[](K&& k){
    auto it = find(k);
    if (it != end()) return it->second;
    storage.emplace_back( std::forward<K>(k), Value{} );
    return storage.back().second;
  }
private:
  /** @brief Return a predicate matching key @p k. @param k Key to match against. @return Lambda matching pairs whose first element equals @p k. */
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  auto key_match( K& k ) {
    return [&k](const std::pair<Key, Value>& kv){
      return kv.first == k;
    };
  }
public:
  /**
   * @brief Find the element with key @p k.
   * @param k  Key to search for.
   * @return   Iterator to the element, or @c end() if not found.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  iterator find(K&& k) {
    return std::find_if( begin(), end(), key_match(k) );
  }
  /**
   * @brief Find the element with key @p k (const overload).
   * @param k  Key to search for.
   * @return   Const iterator to the element, or @c end() if not found.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K, Key>{}>>
  const_iterator find(K&& k) const {
    return const_cast<flat_map*>(this)->find(k);
  }
  /**
   * @brief Remove the element at @p it.
   * @param it  Iterator to the element to remove.
   * @return    Iterator to the element following the removed one.
   */
  iterator erase(const_iterator it) {
    return storage.erase(it);
  }
  /**
   * @brief Insert or update a key-value pair.
   * @param value  Key-value pair to insert or update.
   */
  template<class P, class=std::enable_if_t<std::is_convertible<P,std::pair<Key, Value>>{}>>
  void insert( P&& value )
  {
    auto it = find(value.first);
    if (it == end())
      storage.emplace_back( std::forward<P>(value) );
    else
      it->second = value.second;
  }

  /**
   * @brief Compare two flat_maps for equality.
   * @param rhs  Right-hand side map.
   * @return     @c true if both maps contain the same key-value pairs.
   */
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
  
  /**
   * @brief Lexicographic less-than comparison (order-independent on keys).
   * @param rhs  Right-hand side map.
   * @return     @c true if this map is less than @p rhs.
   */
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
  /**
   * @brief @c std::hash specialisation for @c flat_map, enabling use as an unordered container key.
   */
  template<class K, class V, template<class...> class Storage>
  struct hash<flat_map<K, V, Storage>> {
    /**
     * @brief Compute the hash of a @c flat_map by XOR-folding element hashes.
     * @param key  The map to hash.
     * @return     Combined hash value.
     */
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
