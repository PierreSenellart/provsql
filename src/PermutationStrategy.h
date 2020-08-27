// Taken and adapted from https://github.com/smaniu/treewidth

#ifndef PermutationStrategy_h
#define PermutationStrategy_h

#include <boost/heap/fibonacci_heap.hpp>

#include "Graph.h"

//Class for computing the permutation
class PermutationStrategy{
protected:
  struct node_type{
    unsigned long id;
    unsigned long val;
    bool operator<(const node_type &a) const{
      return val>a.val?true:(val<a.val?false:id>a.id);
    }
  };
  boost::heap::fibonacci_heap<node_type> queue;
  std::unordered_map<unsigned long,
    boost::heap::fibonacci_heap<node_type>::handle_type> queue_nodes;

public:
  //Computes the initial permutation for all nodes in the graph
  void init_permutation(Graph& graph){
    for(auto node:graph.get_nodes()){
      node_type nstruct;
      nstruct.id = node;
      nstruct.val = compute_statistic(node, graph);
      queue_nodes[node]=queue.push(nstruct);
    }
  }

  unsigned long  emptyQ(){
	  node_type nstruct_temp = queue.top();
	  queue.pop();
	  unsigned long node_id = nstruct_temp.id;	  
	  queue_nodes.erase(node_id);
      return queue.size();
  }
  
  //Recomputes the statistic and updates the queue for a subset of nodes
  virtual void recompute(const std::unordered_set<unsigned long> &nodes, Graph& graph){
    for(auto node:nodes){
      node_type nstruct;
      nstruct.id = node;
      nstruct.val = compute_statistic(node, graph);
      queue.update(queue_nodes[node], nstruct);
    }
  }
  
  //Gets the nodes having the smallest value
  unsigned long get_next(){
    node_type nstruct = queue.top();
    unsigned long node_id = nstruct.id;
    queue.pop();
    queue_nodes.erase(node_id);
    return node_id;
  }
  
  //Gets the nodes having the smallest value without deleting
  unsigned long get_next_wo_delete() {
	  node_type nstruct = queue.top();
	  unsigned long node_id = nstruct.id;
	  return node_id;
  }

  //Gets the nodes having the second smallest value and delete it
  unsigned long get_second_next_delete() {
	  node_type nstruct_temp = queue.top();
	  queue.pop();
	  node_type nstruct = queue.top();
	  unsigned long node_id = nstruct.id;
	  queue.pop();
	  queue_nodes.erase(node_id);
	  queue.push(nstruct_temp);
	  return node_id;
  }

  //Gets the nodes having the second smallest value
  unsigned long get_second_next() {
	  node_type nstruct_temp = queue.top();
	  queue.pop();
	  node_type nstruct = queue.top();
	  unsigned long node_id = nstruct.id;
	  queue.push(nstruct_temp);
	  return node_id;
  }
  unsigned long Q_siz(){return queue.size();}
  bool empty_but1() { return !(queue.size()>1); }
  bool empty() {return !(queue.size()>0);}
  
protected:
  //Computes the statistic for a node -- needs to be implemented by subclasses
  unsigned long compute_statistic(unsigned long node, Graph& graph)
  {
    if(graph.has_neighbours(node)){
      return graph.get_neighbours(node).size();
    }
    else{
      return 0;
    }
  }
};

#endif /* PermutationStrategy_h */
