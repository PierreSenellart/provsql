#ifndef TREE_DECOMPOSITION_H
#define TREE_DECOMPOSITION_H

#include <iostream>
#include <string>
#include <vector>

template<unsigned W>
class TreeDecomposition {
 private:
  struct Bag {
    unsigned long gates[W];
    unsigned nb_gates;
  };
 
  std::vector<Bag> bags;
  std::vector<bool> leaves;
  std::vector<unsigned long> parent;
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
};

template<unsigned W>
std::istream& operator>>(std::istream& in, TreeDecomposition<W> &td);

#endif /* TREE_DECOMPOSITION_H */
