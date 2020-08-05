#include <cassert>
#include <fstream>
#include <set>
#include <algorithm>

#include "TreeDecomposition.h"
#include "BooleanCircuit.h"

#include "dDNNF.hpp"

#define MAX_TREEWIDTH 5

template<unsigned W>
TreeDecomposition<W>::TreeDecomposition(std::istream &in)
{
  in >> *this;
}

// This utility function looks for an existing bag to attach a new bag
// that contains a single gate v
template<unsigned W>
unsigned long TreeDecomposition<W>::findGateConnection(unsigned long v) const
{
  for(auto b: bags)
    for(unsigned i=0;i<b.nb_gates;++i) {
      auto g=b.gates[i];
      if(g == v)
        return v;
    }

  return root;
}

// Transform a tree decomposition into one that is root-friendly for a
// given node root, as per the definition page 6 of
// https://arxiv.org/pdf/1811.02944 ; the transformation implemented is
// described in Lemma 2.2 of that paper. The only difference is that we
// do not enforce the tree to be full, as this is not required for
// correctness.
template<unsigned W> void TreeDecomposition<W>::makeFriendly(unsigned
    long v) {
  // Look for a bag root_connection to attach to the new root
  unsigned long root_connection = findGateConnection(v);

  // Create the new root and attach it to this root_connection
  unsigned long new_root = addEmptyBag(root_connection);
  addGateToBag(v, new_root);
  reroot(new_root);

  // Make the tree binary
  for(unsigned i=0, nb_bags=bags.size(); i<nb_bags; ++i) {
    if(children[i].size()<=2)
      continue;

    unsigned long current = i;
    auto copy_children=children[i];
    for(unsigned j=2; j<copy_children.size(); ++j) {
      current = addEmptyBag(parent[current], { current, children[i][j] } );
      for(unsigned k=0; k<bags[i].nb_gates; ++k)
        addGateToBag(bags[i].gates[k], current);
    }
  }

  // Transform leaves into paths that introduce gates one at a time
  for(unsigned i=0, nb_bags=bags.size(); i<nb_bags; ++i) {
    if(children[i].empty()) {
      unsigned long p = i;
      for(unsigned j = 1; j < bags[i].nb_gates; ++j) {
        p = addEmptyBag(p);
        for(unsigned k = 0; k < bags[i].nb_gates - j; ++k)
          addGateToBag(bags[i].gates[k], p);
      }
    }
  }
  
  // Construct for each bag the union of gates in its children
  std::vector<std::set<unsigned long>> gates_in_children(bags.size());
  for(unsigned i=0; i<bags.size(); ++i) {
    if(i!=root) {
      for(unsigned j=0;j<bags[i].nb_gates;++j)
        gates_in_children[parent[i]].insert(bags[i].gates[j]);
    }
  }

  // For every gate that is in an internal bag but not in the union of
  // its children, construct a subtree introducing these gates one at a
  // time
  for(unsigned i=0, nb_bags=bags.size(); i<nb_bags; ++i) {
    if(!children[i].empty()) {
      unsigned nb_gates=0;
      unsigned long intersection[W];
      std::vector<unsigned long> extra_gates;
      for(unsigned j=0; j<bags[i].nb_gates;++j) {
        auto g = bags[i].gates[j];
        if(gates_in_children[i].find(g)==gates_in_children[i].end())
          extra_gates.push_back(g);
        else {
          intersection[nb_gates]=g;
          ++nb_gates;
        }
      }

      if(!extra_gates.empty()) {
        for(unsigned j=0;j<nb_gates;++j)
          bags[i].gates[j]=intersection[j];
        bags[i].nb_gates=nb_gates;

        if(children[i].size()==1 && nb_gates==gates_in_children[i].size()) {
          // We can skip one level, to avoid creating a node identical to
          // the single child
        
          unsigned long new_bag = addEmptyBag(i);
          unsigned gate = extra_gates.back();
          addGateToBag(gate, new_bag);
          addGateToBag(gate, i);
          extra_gates.pop_back();
        }

        unsigned long b = i;
        for(auto g: extra_gates) {
          unsigned long id = addEmptyBag(parent[i], {b});
          for(unsigned long j=0; j < nb_gates; ++j)
            addGateToBag(bags[i].gates[j], id);
          addGateToBag(g, id);

          unsigned long single_gate_bag = addEmptyBag(id);
          addGateToBag(g, single_gate_bag);
          
          gates_in_children[i].insert(g);
          b = id;
        }
      }
    }
  }
}

template<unsigned W>
unsigned long TreeDecomposition<W>::addEmptyBag(unsigned long p, 
                                                const std::vector<unsigned long> &ch)
{
  unsigned long id = bags.size();
  bags.push_back(Bag());
  parent.push_back(p);
  children[p].push_back(id);
  children.push_back(ch);

  for(auto c: ch) {
    if(c!=root)
      children[parent[c]].erase(std::find(children[parent[c]].begin(),
                                          children[parent[c]].end(),
                                          c));
    parent[c] = id;
  }

  return id;
}

template<unsigned W>
void TreeDecomposition<W>::addGateToBag(unsigned long g, unsigned long b)
{
  bags[b].gates[bags[b].nb_gates]=g;
  ++bags[b].nb_gates;
}

template<unsigned W>
void TreeDecomposition<W>::reroot(unsigned long bag)
{
  if(bag == root)
    return;

  for(unsigned long b = bag, p = parent[b], gp; b != root; b = p, p = gp) {
    gp = parent[p];
    parent[p] = b;
    if(p!=root)
      children[gp].erase(std::find(children[gp].begin(),
                                  children[gp].end(),
                                  p));
    children[b].push_back(p);
  }

  children[parent[bag]].erase(std::find(children[parent[bag]].begin(),
                                        children[parent[bag]].end(),
                                        bag));
  parent[bag] = bag;
  root = bag;
}

template<unsigned W>
std::string TreeDecomposition<W>::toDot() const
{
  std::string result="digraph circuit{\n graph [rankdir=UD] ;\n";

  for(unsigned long i=0; i < bags.size(); ++i)
  {
    result += " " + std::to_string(i) + " [label=\"{";
    bool first=true;
    for(unsigned j=0; j<bags[i].nb_gates; ++j) {
      auto gate = bags[i].gates[j];
      if(!first)
        result+=",";
      else
        first=false;
      result += std::to_string(gate);
    }
    result += "}\"];\n";

    if(i!=root)
      result+=" " + std::to_string(parent[i]) + " -> " + std::to_string(i) + ";\n";
  }

  result += "}\n";

  return result;
}

template<unsigned W>
std::istream& operator>>(std::istream& in, TreeDecomposition<W> &td)
{
  in >> td.treewidth;
  assert(td.treewidth <= MAX_TREEWIDTH);

  unsigned long nb_bags;
  in >> nb_bags;
   
  td.bags.resize(nb_bags);
  td.parent.resize(nb_bags);
  td.children.resize(nb_bags);

  for(unsigned long i=0; i<nb_bags; ++i) {
    unsigned long id_bag;
    in >> id_bag;

    assert(i==id_bag);

    in >> td.bags[i].nb_gates;

    assert(td.bags[i].nb_gates <= td.treewidth+1);

    for(unsigned long j=0; j<td.bags[i].nb_gates; ++j) {
      unsigned long g;
      in >> g;

      td.bags[i].gates[j] = g;
    }
    
    unsigned long parent;
    in >> parent;
    td.parent[i] = parent;
    if(parent == i)
      td.root = i;
    else
      td.children[parent].push_back(i);

    unsigned long nb_children;
    in >> nb_children;

    for(unsigned long j=0; j<nb_children; ++j) {
      unsigned long child;
      in >> child;

      // Ignored, we used the parent link instead
    }
  }

  return in;
}

#include <sys/time.h>

static double get_timestamp ()
{
  struct timeval now;
  gettimeofday (&now, NULL);
  return now.tv_usec * 1e-6 + now.tv_sec;
}


int main(int argc, char **argv) {
  if(argc != 2) {
    std::cerr << "Usage: " << argv[0] << " tree_dec" << std::endl;
    exit(1);
  }

  std::ifstream f(argv[1]);

  TreeDecomposition<MAX_TREEWIDTH + 1> td(f);

  BooleanCircuit c;
  c.setGate("0", BooleanGate::OR);
  c.setGate("1", BooleanGate::AND);
  c.setGate("2", BooleanGate::IN, 0.25);
  c.setGate("3", BooleanGate::IN, 0.25);
  c.setGate("4", BooleanGate::IN, 0.25);
  c.setGate("5", BooleanGate::IN, 0.25);
  c.setGate("6", BooleanGate::IN, 0.25);
  c.addWire(0,1);
  c.addWire(0,2);
  c.addWire(1,3);
  c.addWire(1,4);
  c.addWire(1,5);
  c.addWire(1,6);

  double t0, t1;
  t0 = get_timestamp();
  dDNNF dnnf(c, "0", td);
  t1 = get_timestamp();
  std::cerr << "Took " << (t1-t0) << "s" << std::endl;

  std::cerr << dnnf.toString(dnnf.getGate("root")) << std::endl;
  std::cerr << dnnf.dDNNFEvaluation(dnnf.getGate("root")) << std::endl;

  return 0;
}
