/**
 * @file PermutationStrategy.h
 * @brief Priority-queue-based node-elimination ordering for tree decomposition.
 *
 * Originally taken and adapted from https://github.com/smaniu/treewidth
 *
 * Computing an optimal tree decomposition is NP-hard in general.
 * @c PermutationStrategy implements a greedy heuristic that eliminates
 * nodes from a graph one at a time in the order of a **priority** determined
 * by a node statistic (by default, current degree).  The sequence of
 * eliminated nodes defines an **elimination ordering**, and the cliques
 * formed during elimination define the bags of the resulting tree
 * decomposition.
 *
 * The base class implements the **min-degree** heuristic.  Subclasses
 * can override @c compute_statistic() to implement other heuristics
 * (e.g., min-fill, which counts the number of edges that would be added
 * to the graph when the node is eliminated).
 *
 * A Fibonacci heap is used so that @c recompute() (decrease-key after
 * neighbours are updated) runs in amortised O(1) time.
 */
#ifndef PermutationStrategy_h
#define PermutationStrategy_h

#include <boost/heap/fibonacci_heap.hpp>

#include "Graph.h"

/**
 * @brief Node-elimination ordering strategy using a priority queue.
 *
 * Maintains a Fibonacci heap of nodes prioritised by a user-defined
 * statistic.  The main consumer is @c TreeDecomposition, which calls
 * @c init_permutation() once and then repeatedly calls @c get_next() to
 * obtain the next node to eliminate.
 */
class PermutationStrategy{
protected:
  /**
   * @brief Entry in the priority queue.
   *
   * Nodes with a smaller @c val are eliminated first.  Ties are broken
   * by node ID (smaller ID first) to ensure a deterministic ordering.
   */
  struct node_type{
    unsigned long id;   ///< Node identifier
    unsigned long val;  ///< Priority statistic (smaller ⇒ higher priority)
    /**
     * @brief Compare two entries; lower @c val (or lower @c id on tie) wins.
     * @param a  Other entry to compare against.
     * @return   @c true if this entry has higher priority (smaller val or id).
     */
    bool operator<(const node_type &a) const{
      return val>a.val?true:(val<a.val?false:id>a.id);
    }
  };
  /** @brief The priority queue holding all not-yet-eliminated nodes. */
  boost::heap::fibonacci_heap<node_type> queue;
  /** @brief Maps node IDs to their heap handles for O(1) key updates. */
  std::unordered_map<unsigned long,
    boost::heap::fibonacci_heap<node_type>::handle_type> queue_nodes;

public:
  /**
   * @brief Populate the priority queue with all nodes in @p graph.
   *
   * Must be called once before any calls to @c get_next().
   *
   * @param graph  The graph whose nodes should be enqueued.
   */
  void init_permutation(Graph& graph){
    for(auto node:graph.get_nodes()){
      node_type nstruct;
      nstruct.id = node;
      nstruct.val = compute_statistic(node, graph);
      queue_nodes[node]=queue.push(nstruct);
    }
  }

  /**
   * @brief Pop and discard the top node, returning the new queue size.
   *
   * @return Number of remaining nodes after the pop.
   */
  unsigned long  emptyQ(){
	  node_type nstruct_temp = queue.top();
	  queue.pop();
	  unsigned long node_id = nstruct_temp.id;
	  queue_nodes.erase(node_id);
      return queue.size();
  }

  /**
   * @brief Recompute statistics for a subset of nodes and update the queue.
   *
   * Called after a node is eliminated to refresh the priorities of its
   * former neighbours.
   *
   * @param nodes  Set of node IDs whose statistics need updating.
   * @param graph  The current (modified) graph.
   */
  virtual void recompute(const std::unordered_set<unsigned long> &nodes, Graph& graph){
    for(auto node:nodes){
      node_type nstruct;
      nstruct.id = node;
      nstruct.val = compute_statistic(node, graph);
      queue.update(queue_nodes[node], nstruct);
    }
  }

  /**
   * @brief Pop and return the node with the smallest statistic.
   * @return ID of the eliminated node.
   */
  unsigned long get_next(){
    node_type nstruct = queue.top();
    unsigned long node_id = nstruct.id;
    queue.pop();
    queue_nodes.erase(node_id);
    return node_id;
  }

  /**
   * @brief Peek at the node with the smallest statistic (without removing it).
   * @return ID of the top node.
   */
  unsigned long get_next_wo_delete() {
	  node_type nstruct = queue.top();
	  unsigned long node_id = nstruct.id;
	  return node_id;
  }

  /**
   * @brief Pop the top node, then pop and return the new top node.
   *
   * The first (smallest) node is temporarily removed, the second-smallest
   * is popped and returned, and the first is re-inserted.
   *
   * @return ID of the second-smallest node.
   */
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

  /**
   * @brief Peek at the node with the second-smallest statistic (no removal).
   *
   * Temporarily pops the top, peeks at the new top, then re-inserts.
   *
   * @return ID of the second-smallest node.
   */
  unsigned long get_second_next() {
	  node_type nstruct_temp = queue.top();
	  queue.pop();
	  node_type nstruct = queue.top();
	  unsigned long node_id = nstruct.id;
	  queue.push(nstruct_temp);
	  return node_id;
  }

  /**
   * @brief Return the current number of nodes in the queue.
   * @return Number of nodes remaining in the priority queue.
   */
  unsigned long Q_siz(){return queue.size();}
  /**
   * @brief Return @c true if the queue contains exactly one node.
   * @return @c true iff exactly one node remains.
   */
  bool empty_but1() { return !(queue.size()>1); }
  /**
   * @brief Return @c true if the queue is empty.
   * @return @c true iff no nodes remain.
   */
  bool empty() {return !(queue.size()>0);}

protected:
  /**
   * @brief Compute the priority statistic for @p node in @p graph.
   *
   * The default implementation returns the degree (number of neighbours)
   * of the node.  Override in subclasses to implement other heuristics.
   *
   * @param node   Node whose statistic is needed.
   * @param graph  Current graph state.
   * @return       Non-negative statistic value (smaller ⇒ eliminate sooner).
   */
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
