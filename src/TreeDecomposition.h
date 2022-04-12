#ifndef TREE_DECOMPOSITION_H
#define TREE_DECOMPOSITION_H

#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
#include <boost/container/static_vector.hpp>

#include "flat_set.hpp"
#include "BooleanCircuit.h"

// Forward declaration for friend
class dDNNFTreeDecompositionBuilder;

enum class bag_t : size_t {};

class TreeDecomposition {
 public:
  static constexpr int MAX_TREEWIDTH = 10;
  static constexpr int OPTIMAL_ARITY = 2;

  template<class T>
    using small_vector = boost::container::static_vector<T, MAX_TREEWIDTH+1>;

  using Bag = flat_set<gate_t, small_vector>;
 
 private:
  std::vector<Bag> bags;
  std::vector<bag_t> parent;
  std::vector<std::vector<bag_t>> children;
  bag_t root;
  unsigned treewidth;

  TreeDecomposition() = default;
  
  bag_t findGateConnection(gate_t v) const;
  void reroot(bag_t bag);
  bag_t addEmptyBag(bag_t parent, const std::vector<bag_t> &children = std::vector<bag_t>());
  void addGateToBag(gate_t g, bag_t b);

  Bag &getBag(bag_t b) 
    { return bags[static_cast<std::underlying_type<bag_t>::type>(b)]; }
  const Bag &getBag(bag_t b) const
    { return bags[static_cast<std::underlying_type<bag_t>::type>(b)]; }
  std::vector<bag_t> &getChildren(bag_t b) 
    { return children[static_cast<std::underlying_type<bag_t>::type>(b)]; }
  const std::vector<bag_t> &getChildren(bag_t b) const
    { return children[static_cast<std::underlying_type<bag_t>::type>(b)]; }
  bag_t getParent(bag_t b) const
    { return parent[static_cast<std::underlying_type<bag_t>::type>(b)]; }
  void setParent(bag_t b, bag_t p)
    { parent[static_cast<std::underlying_type<bag_t>::type>(b)]=p; }

 public:
  TreeDecomposition(const BooleanCircuit &bc);
  TreeDecomposition(std::istream &in);
  TreeDecomposition(const TreeDecomposition &td);
  TreeDecomposition &operator=(const TreeDecomposition &td);

  unsigned getTreewidth() const { return treewidth; }
  void makeFriendly(gate_t root);

  std::string toDot() const;
  
  friend std::istream& operator>>(std::istream& in, TreeDecomposition &td);
  friend class dDNNFTreeDecompositionBuilder;
};

std::istream& operator>>(std::istream& in, TreeDecomposition &td);

inline bag_t &operator++(bag_t &b) {
  return b=bag_t{static_cast<std::underlying_type<bag_t>::type>(b)+1};
}
inline bag_t operator++(bag_t &b, int) {
  auto temp{b};
  b=bag_t{static_cast<std::underlying_type<bag_t>::type>(b)+1};
  return temp;
}

inline bool operator<(bag_t t, std::vector<bag_t>::size_type u)
{
  return static_cast<std::underlying_type<bag_t>::type>(t)<u;
}

inline std::string to_string(bag_t b) {
  return std::to_string(static_cast<std::underlying_type<bag_t>::type>(b));
}

inline std::istream &operator>>(std::istream &i, bag_t &b)
{
  std::underlying_type<bag_t>::type u;
  i >> u;
  b=bag_t{u};
  return i;
}

namespace std {
  template<>
  struct hash<gate_t>
  {
    size_t operator()(gate_t g) const
    {
      return hash<typename std::underlying_type<gate_t>::type>()(
          static_cast<typename std::underlying_type<gate_t>::type>(g));
    }
  };
};

class TreeDecompositionException : public std::exception {};

#endif /* TREE_DECOMPOSITION_H */
