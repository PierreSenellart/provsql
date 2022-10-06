#include <cassert>
#include <set>
#include <algorithm>
#include <string>
#include <type_traits>

#include "TreeDecomposition.h"
#include "BooleanCircuit.h"
#include "PermutationStrategy.h"
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
// correctness; and we do not make it binary but n-ary for a small n, as
// it is more efficient.
void TreeDecomposition::makeFriendly(gate_t v) {
  // Look for a bag root_connection to attach to the new root
  auto root_connection = findGateConnection(v);

  // Create the new root and attach it to this root_connection
  auto new_root = addEmptyBag(root_connection);
  addGateToBag(v, new_root);
  reroot(new_root);

  // Make the tree n-ary for a small n
  auto nb_bags = bags.size();
  for(bag_t i{0}; i<nb_bags; ++i) {
    if(getChildren(i).size()<=OPTIMAL_ARITY)
      continue;

    auto current = i;
    auto copy_children=getChildren(i);
    for(int j=copy_children.size()-OPTIMAL_ARITY-1; j>=0; j-=OPTIMAL_ARITY-1) {
      decltype(copy_children) new_children;
      new_children.push_back(current);
      for(auto k{j}; k>=0 && k>j-OPTIMAL_ARITY; --k)
        new_children.push_back(copy_children[k]);
      current = addEmptyBag(getParent(current), new_children);
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

// Taken and adapted from https://github.com/smaniu/treewidth
TreeDecomposition::TreeDecomposition(const BooleanCircuit &bc)
{
  Graph graph(bc);
      
  PermutationStrategy strategy;

  strategy.init_permutation(graph);

  // Upper bound on size of the bags vector to avoid redimensioning
  bags.reserve(graph.number_nodes());

  unsigned max_width{0};
  std::unordered_map<gate_t, bag_t> bag_ids;
  bag_t bag_id{0};

  //looping greedily through the permutation
  //we stop when the maximum bag has the same width as the remaining graph
  //or when we achive the partial decomposition condition
  while(max_width<graph.number_nodes() && !strategy.empty()){
    //getting the next node
    unsigned long node = strategy.get_next();
    //removing the node from the graph and getting its neighbours
    std::unordered_set<unsigned long> neigh = graph.remove_node(node);
    max_width = std::max<unsigned>(neigh.size(), max_width);
    //we stop as soon as we find bag that is 
    if(max_width>MAX_TREEWIDTH)
      throw TreeDecompositionException();

    //filling missing edges between the neighbours and recomputing statistics
    //  for relevant nodes in the graph (the neighbours, most of the time)
    graph.fill(neigh);
    strategy.recompute(neigh, graph);

    Bag bag;
    for(auto n: neigh) {
      bag.insert(gate_t{n});
    }
    bag.insert(gate_t{node});
      
    bag_ids[gate_t{node}] = bag_id++;

    bags.push_back(bag);
  }
 
  if(graph.get_nodes().size()>MAX_TREEWIDTH)
    throw TreeDecompositionException();

  if(graph.number_nodes()>0) {
    Bag remaining_bag; 
    for(auto n: graph.get_nodes()) {
      remaining_bag.insert(gate_t{n});
    }
    bags.push_back(remaining_bag);
  }

  parent.resize(bags.size());
  children.resize(bags.size());

  if(graph.number_nodes()==0)
    treewidth=max_width;
  else
    treewidth = std::max<unsigned>(max_width,graph.number_nodes()-1);
    
  for(bag_t i{0};i<bags.size()-1;++i){
    bag_t min_bag{bags.size()-1};
    for(auto n: getBag(i)) {
      auto it = bag_ids.find(n);
      if(it!=bag_ids.end() && it->second!=i)
        min_bag = std::min(it->second, min_bag);
    }
    setParent(i, min_bag);
    getChildren(min_bag).push_back(i);
  }

  // Special semantics: a node is its own parent if it is the root
  root = bag_t{bags.size()-1};
  setParent(root,root);
}
