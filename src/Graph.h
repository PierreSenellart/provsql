/**
 * @file Graph.h
 * @brief Undirected graph used in tree-decomposition computations.
 *
 * Originally taken and adapted from https://github.com/smaniu/treewidth
 *
 * @c Graph is a mutable, adjacency-list-based undirected (or directed)
 * graph over @c unsigned @c long node IDs.  It is used during the
 * tree-decomposition algorithm to represent the "primal graph" of a
 * @c BooleanCircuit: nodes correspond to gates and edges connect gates
 * that are connected by a wire.
 *
 * The mutating operations (@c remove_node, @c fill, @c contract_edge)
 * are used by the elimination-ordering heuristic implemented in
 * @c PermutationStrategy and @c TreeDecomposition.
 */
#ifndef Graph_h
#define Graph_h
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <cassert>

#include "BooleanCircuit.h"

/**
 * @brief Mutable adjacency-list graph over unsigned-long node IDs.
 *
 * Supports both directed and undirected edges, node/edge removal,
 * clique-fill operations, and edge contraction, as needed by the
 * min-fill tree-decomposition algorithm.
 */
class Graph {
private:
std::unordered_map<unsigned long, std::unordered_set<unsigned long> > adj_list; ///< Adjacency lists
std::unordered_set<unsigned long> node_set; ///< Set of all node IDs
unsigned long num_edges = 0; ///< Current edge count

public:
/**
 * @brief Construct the primal graph of a @c BooleanCircuit.
 *
 * Each gate (except @c UNDETERMINED and @c MULVAR) becomes a node.
 * Each wire between two gates becomes an undirected edge.
 *
 * @param bc  The Boolean circuit whose structure defines the graph.
 */
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

/**
 * @brief Add an edge between @p src and @p tgt.
 *
 * If the edge already exists the call is a no-op.  Both endpoint nodes
 * are added to @c node_set if not already present.
 *
 * @param src        Source node.
 * @param tgt        Target node.
 * @param undirected If @c true, also add the reverse edge.
 */
void add_edge(unsigned long src, unsigned long tgt, bool undirected=true){
  if(!has_edge(src, tgt)) {
    node_set.insert(src);
    node_set.insert(tgt);
    adj_list[src].insert(tgt);
    if(undirected) adj_list[tgt].insert(src);
    num_edges++;
  }
};

/**
 * @brief Add @p node to the graph (no edges).
 * @param node  Node ID to insert.
 */
void add_node(unsigned long node){
  node_set.insert(node);
}

/**
 * @brief Remove @p node and all its incident edges.
 *
 * @param node  Node ID to remove.
 * @return      The adjacency set of @p node before removal.
 */
std::unordered_set<unsigned long> remove_node(unsigned long node){
  node_set.erase(node);
  for(auto neighbour:adj_list[node]) {
    adj_list[neighbour].erase(node);
    num_edges--;
  }
  auto it = adj_list.find(node);
  auto adjacency_list = std::move(it->second);
  adj_list.erase(it);
  return adjacency_list;
}

/**
 * @brief Test whether two nodes share more than @p k-1 common neighbours.
 *
 * Used by the min-fill heuristic to decide whether eliminating a node
 * improves the treewidth bound.
 *
 * @param k   Treewidth bound being tested.
 * @param n1  First node.
 * @param n2  Second node.
 * @return    @c true if the common-neighbour count exceeds @p k-1.
 */
bool neighbour_improved(unsigned k,unsigned long n1, unsigned long n2){
  bool retval = false;
  auto &neigh1 = get_neighbours(n1);
  auto &neigh2 = get_neighbours(n2);

  unsigned long count = 0;
  if      (neigh1.size()>k-1 && neigh2.size()>k-1) {
    for (auto nn1:neigh1) {
      for (auto nn2:neigh2) {
        if (nn1==nn2) {
          count = count+1;
          break;
        }

      }
    }
  }
  if (count > k-1) {
    retval = true;
  }

  return retval;
}

/**
 * @brief Add all missing edges within @p nodes (clique fill).
 *
 * Connects every pair of nodes in @p nodes that is not already connected,
 * making the subgraph induced by @p nodes into a clique.
 *
 * @param nodes      Set of node IDs to fill.
 * @param undirected If @c true, add edges in both directions.
 */
void fill(const std::unordered_set<unsigned long>& nodes, \
          bool undirected=true){
  for(auto src: nodes)
    for(auto tgt: nodes)
      if(undirected) {
        if(src<tgt)
          add_edge(src, tgt, undirected);
      }
      else{
        if(src!=tgt)
          add_edge(src, tgt, undirected);
      }

}

/**
 * @brief Contract the edge (src, tgt) by merging @p tgt into @p src.
 *
 * All edges from @p tgt are redirected to @p src, then @p tgt is removed.
 *
 * @param src  The node that survives the contraction.
 * @param tgt  The node to be merged into @p src.
 */
void contract_edge(unsigned long src, unsigned long tgt){
  for(auto v:get_neighbours(tgt))
    if((v!=src)&&!has_edge(src,v)) add_edge(src,v);
  remove_node(tgt);
}

/**
 * @brief Return @c true if @p node has any adjacent edges.
 * @param node  Node to query.
 * @return      @c true if the adjacency list contains an entry for @p node.
 */
bool has_neighbours(unsigned long node) const {
  return adj_list.find(node)!=adj_list.end();
}

/**
 * @brief Return @c true if @p node is present in the graph.
 * @param node  Node to query.
 * @return      @c true if @p node exists in the node set.
 */
bool has_node(unsigned long node) const {
  return node_set.find(node)!=node_set.end();
}

/**
 * @brief Return @c true if a directed edge from @p src to @p tgt exists.
 * @param src  Source node.
 * @param tgt  Target node.
 * @return     @c true if the edge @p src → @p tgt is present.
 */
bool has_edge(unsigned long src, unsigned long tgt) {
  bool retval = false;
  if(has_neighbours(src)) {
    auto &neigh = get_neighbours(src);
    retval = neigh.find(tgt)!=neigh.end();
  }
  return retval;
}

/**
 * @brief Return the neighbour set of @p node.
 *
 * @p node must be present in the graph (asserted in debug builds).
 *
 * @param node  Node to query.
 * @return      Const reference to the adjacency set.
 */
const std::unordered_set<unsigned long> &get_neighbours(unsigned long node) const {
  assert(has_node(node));

  return (adj_list.find(node))->second;
}

/**
 * @brief Return the set of all node IDs in the graph.
 * @return Const reference to the node set.
 */
const std::unordered_set<unsigned long> &get_nodes() const {
  return node_set;
}

/**
 * @brief Return the number of nodes in the graph.
 * @return Total node count.
 */
unsigned long number_nodes() const {
  return node_set.size();
}

/**
 * @brief Return the number of edges in the graph.
 * @return Total edge count.
 */
unsigned long number_edges() const {
  return num_edges;
}
};


#endif /* Graph_h */
