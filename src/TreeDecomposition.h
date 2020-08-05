#ifndef TREE_DECOMPOSITION_H
#define TREE_DECOMPOSITION_H

#include <iostream>
#include <string>
#include <vector>

#include "dDNNF.h"

template<unsigned W>
class TreeDecomposition {
 public:
  struct Bag {
    unsigned long gates[W];
    unsigned nb_gates;
  };
 
 private:
  std::vector<Bag> bags;
  std::vector<unsigned long> parent;
  std::vector<std::vector<unsigned long>> children;
  unsigned long root;
  unsigned long treewidth;

  TreeDecomposition() = default;
  
  unsigned long findGateConnection(unsigned long v) const;
  void reroot(unsigned long bag);
  unsigned long addEmptyBag(unsigned long parent, const std::vector<unsigned long> &children = std::vector<unsigned long>());
  void addGateToBag(unsigned long g, unsigned long b);

 public:
  TreeDecomposition(std::istream &in);
  TreeDecomposition(const TreeDecomposition &td);
  TreeDecomposition &operator=(const TreeDecomposition &td);

  void makeFriendly(unsigned long root);

  std::string toDot() const;
  
  template<unsigned X>
  friend std::istream& operator>>(std::istream& in, TreeDecomposition<X> &td);

  friend class dDNNF;
};

template<unsigned W>
std::istream& operator>>(std::istream& in, TreeDecomposition<W> &td);

#endif /* TREE_DECOMPOSITION_H */
