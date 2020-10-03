// Taken and adapted from https://github.com/smaniu/treewidth

#ifndef Graph_h
#define Graph_h
#include <stdlib.h>
#include <unordered_map>
#include <unordered_set>
#include <cassert>

#include "BooleanCircuit.h"

//Class for the graph
class Graph{
private:
  std::unordered_map<unsigned long, std::unordered_set<unsigned long>> adj_list;
  std::unordered_set<unsigned long> node_set;
  unsigned long num_edges = 0;
  
public:
  Graph(const BooleanCircuit &bc)
  {
    for(gate_t g1{0}; g1<bc.getNbGates(); ++g1) {
      // We do not take into account these gates, which have no purpose
      // in the circuit
      if(bc.getGateType(g1) == BooleanGate::UNDETERMINED || bc.getGateType(g1) == BooleanGate::MULVAR)
        continue;

      add_node(static_cast<unsigned long>(g1));
      for(auto g2: bc.getWires(g1))
        add_edge(static_cast<unsigned long>(g1), static_cast<unsigned long>(g2), true);
    }
  }

  void add_edge(unsigned long src, unsigned long tgt, bool undirected=true){
    if(!has_edge(src, tgt)){
      node_set.insert(src);
      node_set.insert(tgt);
      adj_list[src].insert(tgt);
      if(undirected) adj_list[tgt].insert(src);
      num_edges++;
    }
  };
  
  void add_node(unsigned long node){
    node_set.insert(node);
  }
  
  // Returns adjacency list
  std::unordered_set<unsigned long> remove_node(unsigned long node){
    node_set.erase(node);
    for(auto neighbour:adj_list[node]){
      adj_list[neighbour].erase(node);
      num_edges--;
    }
    auto it = adj_list.find(node);
    auto adjacency_list = std::move(it->second);
    adj_list.erase(it);
    return adjacency_list;
  }

  bool neighbour_improved(unsigned k,unsigned long n1, unsigned long n2){
		bool retval = false;		
		auto &neigh1 = get_neighbours(n1);
		auto &neigh2 = get_neighbours(n2);
			
		unsigned long count = 0;
		if 	(neigh1.size()>k-1 && neigh2.size()>k-1){
			for (auto nn1:neigh1){
				for (auto nn2:neigh2){
					if (nn1==nn2){
						count = count+1;		
						break;
					}
			
				}
			}		
		}
		if (count > k-1){
			retval = true;
		}

		return retval;
}
 
  void fill(const std::unordered_set<unsigned long>& nodes,\
            bool undirected=true){
    for(auto src: nodes)
      for(auto tgt: nodes)
        if(undirected){
          if(src<tgt)
            add_edge(src, tgt, undirected);
        }
        else{
          if(src!=tgt)
            add_edge(src, tgt, undirected);
        }
    
  }
  
  void contract_edge(unsigned long src, unsigned long tgt){
    for(auto v:get_neighbours(tgt))
      if((v!=src)&&!has_edge(src,v)) add_edge(src,v);
    remove_node(tgt);
  }
  
  bool has_neighbours(unsigned long node) const{
    return adj_list.find(node)!=adj_list.end();
  }
  
  bool has_node(unsigned long node) const{
    return node_set.find(node)!=node_set.end();
  }
  
  bool has_edge(unsigned long src, unsigned long tgt) {
    bool retval = false;
    if(has_neighbours(src)){
      auto &neigh = get_neighbours(src);
      retval = neigh.find(tgt)!=neigh.end();
    }
    return retval;
  }
  
  const std::unordered_set<unsigned long> &get_neighbours(unsigned long node) const{
    assert(has_node(node));

    return (adj_list.find(node))->second;
  }
  
  const std::unordered_set<unsigned long> &get_nodes() const{
    return node_set;
  }
  
  unsigned long number_nodes() const{
    return node_set.size();
  }
  
  unsigned long number_edges() const{
    return num_edges;
  }
};


#endif /* Graph_h */
