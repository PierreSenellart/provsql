#ifndef TREE_DECOMPOSITION_H
#define TREE_DECOMPOSITION_H

#include <iostream>
#include <vector>

class TreeDecomposition {
 private:
  struct Bag {
    std::vector<unsigned long> gates;
  };
 
  std::vector<Bag> bags;
  std::vector<unsigned long> leaves;
  std::vector<unsigned long> parent;
  unsigned long root;
  unsigned long treewidth;

  TreeDecomposition() {}

 public:
  TreeDecomposition(std::istream &in);
  TreeDecomposition makeFriendly() const;
  
  friend std::istream& operator>>(std::istream& in, TreeDecomposition &td);
};

std::istream& operator>>(std::istream& in, TreeDecomposition &td);

#endif /* TREE_DECOMPOSITION_H */
