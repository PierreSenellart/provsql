#include <cassert>
#include <fstream>
#include <set>
#include <algorithm>
#include <string>
#include <type_traits>

#include "TreeDecomposition.h"
#include "BooleanCircuit.h"

#include "dDNNFTreeDecompositionBuilder.h"

TreeDecomposition::TreeDecomposition(std::istream &in)
{
  in >> *this;
}

// This utility function looks for an existing bag to attach a new bag
// that contains a single gate v
bag_t TreeDecomposition::findGateConnection(gate_t v) const
{
  for(bag_t i{0}; i<bags.size(); ++i)
    for(auto g: getBag(i)) {
      if(g == v)
        return i;
    }

  return root;
}

// Transform a tree decomposition into one that is root-friendly for a
// given node root, as per the definition page 6 of
// https://arxiv.org/pdf/1811.02944 ; the transformation implemented is
// described in Lemma 2.2 of that paper. The only difference is that we
// do not enforce the tree to be full, as this is not required for
// correctness.
void TreeDecomposition::makeFriendly(gate_t v) {
  // Look for a bag root_connection to attach to the new root
  auto root_connection = findGateConnection(v);

  // Create the new root and attach it to this root_connection
  auto new_root = addEmptyBag(root_connection);
  addGateToBag(v, new_root);
  reroot(new_root);

  // Make the tree binary
  auto nb_bags = bags.size();
  for(bag_t i{0}; i<nb_bags; ++i) {
    if(getChildren(i).size()<=2)
      continue;

    auto current = i;
    auto copy_children=getChildren(i);
    for(int j=copy_children.size()-3; j>=0; --j) {
      current = addEmptyBag(getParent(current), { current, copy_children[j] } );
      for(auto g: getBag(i))
        addGateToBag(g, current);
    }
  }

  // Transform leaves into paths that introduce gates one at a time
  nb_bags = bags.size();
  for(bag_t i{0}; i<nb_bags; ++i) {
    if(getChildren(i).empty()) {
      auto p = i;
      for(size_t j = 1; j < getBag(i).size(); ++j) {
        p = addEmptyBag(p);
        Bag::const_iterator it=getBag(i).begin();
        for(size_t k = 0; k < getBag(i).size() - j; ++k, ++it)
          addGateToBag(*it, p);
      }
    }
  }
  
  // Construct for each bag the union of gates in its children
  std::vector<std::set<gate_t>> gates_in_children(bags.size());
  auto getChildrenGates = [&gates_in_children](bag_t i) -> auto& {
    return gates_in_children[static_cast<std::underlying_type<bag_t>::type>(i)];
  };

  for(bag_t i{0}; i<bags.size(); ++i) {
    if(i!=root) {
      for(auto g: getBag(i))
        getChildrenGates(getParent(i)).insert(g);
    }
  }

  // For every gate that is in an internal bag but not in the union of
  // its children, construct a subtree introducing these gates one at a
  // time
  nb_bags = bags.size();
  for(bag_t i{0}; i<nb_bags; ++i) {
    if(!getChildren(i).empty()) {
      Bag intersection;
      std::vector<gate_t> extra_gates;
      for(auto g: getBag(i)) {
        if(getChildrenGates(i).find(g) == getChildrenGates(i).end())
          extra_gates.push_back(g);
        else 
          intersection.insert(g);
      }

      if(!extra_gates.empty()) {
        getBag(i) = intersection;

        if(getChildren(i).size()==1 && intersection.size()==getChildrenGates(i).size()) {
          // We can skip one level, to avoid creating a node identical to
          // the single child
        
          auto new_bag = addEmptyBag(i);
          auto gate = extra_gates.back();
          addGateToBag(gate, new_bag);
          addGateToBag(gate, i);
          extra_gates.pop_back();
        }

        auto b = i;
        for(auto g: extra_gates) {
          auto id = addEmptyBag(getParent(b), {b});
          getBag(id) = getBag(b);
          addGateToBag(g, id);

          auto single_gate_bag = addEmptyBag(id);
          addGateToBag(g, single_gate_bag);

          b = id;
        }
      }
    }
  }
}

bag_t TreeDecomposition::addEmptyBag(bag_t p, 
                                     const std::vector<bag_t> &ch)
{
  bag_t id {bags.size()};
  bags.push_back(Bag());
  parent.push_back(p);
  getChildren(p).push_back(id);
  children.push_back(ch);

  for(auto c: ch) {
    if(c!=root)
      getChildren(getParent(c)).erase(std::find(getChildren(getParent(c)).begin(),
                                                getChildren(getParent(c)).end(),
                                                c));
    setParent(c,id);
  }

  return id;
}

void TreeDecomposition::addGateToBag(gate_t g, bag_t b)
{
  getBag(b).insert(g);
}

void TreeDecomposition::reroot(bag_t bag)
{
  if(bag == root)
    return;

  for(bag_t b = bag, p = getParent(b), gp; b != root; b = p, p = gp) {
    gp = getParent(p);
    setParent(p, b);
    if(p!=root)
      getChildren(gp).erase(std::find(getChildren(gp).begin(),
                                      getChildren(gp).end(),
                                      p));
    getChildren(b).push_back(p);
  }

  getChildren(getParent(bag)).erase(std::find(getChildren(getParent(bag)).begin(),
                                              getChildren(getParent(bag)).end(),
                                              bag));
  setParent(bag, bag);
  root = bag;
}

std::string TreeDecomposition::toDot() const
{
  std::string result="digraph circuit{\n graph [rankdir=UD] ;\n";

  for(bag_t i{0}; i < bags.size(); ++i)
  {
    result += " " + to_string(i) + " [label=\"{";
    bool first=true;
    for(auto gate: getBag(i)) {
      if(!first)
        result+=",";
      else
        first=false;
      result += to_string(gate);
    }
    result += "}\"];\n";

    if(i!=root)
      result+=" " + to_string(getParent(i)) + " -> " + to_string(i) + ";\n";
  }

  result += "}\n";

  return result;
}

std::istream& operator>>(std::istream& in, TreeDecomposition &td)
{
  in >> td.treewidth;
  assert(td.treewidth <= TreeDecomposition::MAX_TREEWIDTH);

  unsigned long nb_bags;
  in >> nb_bags;
   
  td.bags.resize(nb_bags);
  td.parent.resize(nb_bags);
  td.children.resize(nb_bags);

  for(bag_t i{0}; i<nb_bags; ++i) {
    bag_t id_bag;
    in >> id_bag;

    assert(i==id_bag);

    unsigned nb_gates;
    in >> nb_gates;

    assert(nb_gates <= td.treewidth+1);

    for(unsigned long j=0; j<nb_gates; ++j) {
      gate_t g;
      in >> g;

      td.addGateToBag(g, i);
    }
    
    bag_t parent;
    in >> parent;
    td.setParent(i, parent);
    if(parent == i)
      td.root = i;
    else
      td.getChildren(parent).push_back(i);

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
  if(argc != 3) {
    std::cerr << "Usage: " << argv[0] << " circuit tree_dec" << std::endl;
    exit(1);
  }

  std::ifstream f(argv[2]);
  TreeDecomposition td(f);
  f.close();

  std::ifstream g(argv[1]);
  BooleanCircuit c;
  unsigned nbGates;

  g >> nbGates;
  std::string line;
  std::getline(g,line);

  for(unsigned i=0; i<nbGates;++i) {
    std::getline(g, line);
    if(line=="IN")
      c.setGate(std::to_string(i), BooleanGate::IN, 0.001);
    else
      c.setGate(std::to_string(i), line=="OR"?BooleanGate::OR:BooleanGate::AND);
  }

  gate_t u,v;
  while(g >> u >> v)
    c.addWire(u,v);
  g.close();

  double t0, t1, t2;
  t0 = get_timestamp();

  auto dnnf{dDNNFTreeDecompositionBuilder{c, "0", td}.build()};
  t1 = get_timestamp();
  std::cerr << "Took " << (t1-t0) << "s" << std::endl;

  t2 = get_timestamp();
  std::cerr << "Using dDNNF: " << dnnf.dDNNFEvaluation(dnnf.getGate("root")) << std::endl;
  std::cerr << "Took " << (t2-t1) << "s" << std::endl;

  return 0;
}
