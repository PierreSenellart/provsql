/**
 * @file flat_set.hpp
 * @brief Flat (unsorted, contiguous-storage) set template.
 *
 * Heavily inspired from https://stackoverflow.com/a/30938947, credits to
 * Yakk - Adam Nevraum @ StackOverflow
 *
 * @c flat_set<T, Storage, hash> is a lightweight set container that stores
 * elements in a contiguous sequence.  It is the set analogue of @c flat_map,
 * with the same trade-offs: O(n) membership tests but cache-friendly
 * performance for small sets, and pluggable storage for stack allocation.
 *
 * ProvSQL uses this as the @c suspicious_t type in
 * @c dDNNFTreeDecompositionBuilder (a set of gates bounded by the
 * treewidth + 1 of the circuit), and as the bag type @c Bag in
 * @c TreeDecomposition.
 *
 * A @c std::hash specialisation is provided so that @c flat_set can be
 * used as a key in @c std::unordered_map (via the @c hash template
 * parameter for the element hash).
 */
#ifndef FLAT_SET_H
#define FLAT_SET_H

// Heavily inspired from https://stackoverflow.com/a/30938947, credits to
// Yakk - Adam Nevraum @ StackOverflow

#include <vector>
#include <utility>
#include <algorithm>

/**
 * @brief Flat set with pluggable storage.
 *
 * @tparam T        Element type.  Must be equality-comparable.
 * @tparam Storage  Container template for storage (default: @c std::vector).
 * @tparam hash     Hash functor for elements (used by the @c std::hash
 *                  specialisation, default: @c std::hash<T>).
 */
template<class T, template<class ...>class Storage=std::vector, class hash=std::hash<T> >
struct flat_set {
  using storage_t = Storage<T>; ///< Underlying container type
  using hash_t = hash;          ///< Hash functor type for elements
  storage_t storage;            ///< Contiguous element storage

  using iterator=decltype(std::begin(std::declval<storage_t&>())); ///< Mutable iterator
  using const_iterator=decltype(std::begin(std::declval<const storage_t&>())); ///< Const iterator

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
  /** @brief Return the number of elements in the set. @return Element count. */
  size_t size() const {
    return storage.size();
  }
  /** @brief Return @c true if the set is empty. @return @c true if no elements are stored. */
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
public:
  /**
   * @brief Find an element equal to @p k.
   * @param k  Element to search for.
   * @return   Iterator to the element, or @c end() if not found.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K, T>{}> >
  iterator find(K&& k) {
    return std::find( begin(), end(), k );
  }
  /**
   * @brief Find an element equal to @p k (const overload).
   * @param k  Element to search for.
   * @return   Const iterator to the element, or @c end() if not found.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K, T>{}> >
  const_iterator find(K&& k) const {
    return const_cast<flat_set*>(this)->find(k);
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
   * @brief Insert @p value if not already present (const-ref overload).
   * @param value  Element to insert.
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K,T>{}> >
  void insert( const K& value )
  {
    auto it = find(value);
    if (it == end())
      storage.emplace_back( value );
  }
  /**
   * @brief Insert @p value if not already present (rvalue overload).
   * @param value  Element to insert (moved).
   */
  template<class K, class=std::enable_if_t<std::is_convertible<K,T>{}> >
  void insert( K&& value )
  {
    auto it = find(value);
    if (it == end())
      storage.emplace_back( std::forward<T>(value) );
  }

  /**
   * @brief Compare two flat_sets for equality (order-independent).
   * @param rhs  Right-hand side set.
   * @return     @c true if both sets contain the same elements.
   */
  bool operator==(const flat_set &rhs) const
  {
    if(size()!=rhs.size())
      return false;
    for(auto i: rhs)
      if(find(i)==end())
        return false;

    return true;
  }

  /**
   * @brief Less-than comparison based on minimum differing element.
   * @param rhs  Right-hand side set.
   * @return     @c true if this set is less than @p rhs.
   */
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
/**
 * @brief @c std::hash specialisation for @c flat_set, enabling use as an unordered container key.
 */
template<class T, template<class ...> class Storage, class h>
struct hash<flat_set<T, Storage, h> > {
  /**
   * @brief Compute the hash of a @c flat_set by XOR-folding element hashes.
   * @param key  The set to hash.
   * @return     Combined hash value.
   */
  size_t operator()(const flat_set<T, Storage> &key) const
  {
    size_t result = 0xDEADBEEF;
    for(const auto &i: key)
      result ^= h()(i);
    return result;
  }
};
}
#endif /* FLAT_SET_H */
