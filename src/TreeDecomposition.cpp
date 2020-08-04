#include <cassert>

#include "TreeDecomposition.h"

using namespace std;

TreeDecomposition::TreeDecomposition(istream &in)
{
  in >> *this;
}

TreeDecomposition TreeDecomposition::makeFriendly() const
{
  TreeDecomposition td;

  // TODO
  
  return td;
}

istream& operator>>(istream& in, TreeDecomposition &td)
{
  in >> td.treewidth;

  unsigned long nb_bags;
  in >> nb_bags;
   
  td.bags.resize(nb_bags);
  td.parent.resize(nb_bags);

  for(unsigned long i=0; i<nb_bags; ++i) {
    unsigned long id_bag;
    in >> id_bag;

    assert(i==id_bag);

    unsigned long nb_gates;
    in >> nb_gates;

    td.bags[i].gates.resize(nb_gates);

    for(unsigned long j=0; j<nb_gates; ++j) {
      unsigned long g;
      in >> g;

      td.bags[i].gates[j] = g;
    }
    
    unsigned long parent;
    cin >> parent;
    td.parent[i] = parent;
    if(parent == i)
      td.root = i;

    unsigned long nb_children;
    cin >> nb_children;

    if(nb_children == 0)
      td.leaves.push_back(i);

    for(unsigned long j=0; j<nb_children; ++j) {
      unsigned long child;
      in >> child;

      // Ignored, we used the parent link instead
    }
  }

  return in;
}
