#ifndef dDNNF_TREE_DECOMPOSITION_BUILDER_H
#define dDNNF_TREE_DECOMPOSITION_BUILDER_H

#include <cassert>
#include <unordered_map>
#include <map>

#include <boost/container/static_vector.hpp>

#include "flat_map.hpp"
#include "flat_set.hpp"
#include "TreeDecomposition.h"
#include "dDNNF.h"
#include "BooleanCircuit.h"

class dDNNFTreeDecompositionBuilder
{
 public:
  template<class T>
    using small_vector = boost::container::static_vector<T, TreeDecomposition::MAX_TREEWIDTH+1>;
  using valuation_t = flat_map<gate_t, bool, small_vector>;
  using suspicious_t = flat_set<gate_t, small_vector>;
  template<class T>
    using gate_vector_t = std::vector<T>;
  using gates_to_or_t =
    std::unordered_map<valuation_t, std::map<suspicious_t, gate_vector_t<gate_t>>>;

 private:
  const BooleanCircuit &c;
  gate_t root_id;
  TreeDecomposition &td;
  dDNNF d;
  std::unordered_map<gate_t, bag_t> responsible_bag;
  std::unordered_map<gate_t, gate_t> input_gate;
  std::unordered_map<gate_t, gate_t> negated_input_gate;
  std::set<std::pair<gate_t, gate_t>> wiresSet;
  
  struct dDNNFGate {
    gate_t id;
    valuation_t valuation;
    suspicious_t suspicious;

    dDNNFGate(gate_t i, valuation_t v, suspicious_t s) :
      id{i}, valuation{std::move(v)}, suspicious{std::move(s)} {}
  };

  [[nodiscard]] gates_to_or_t collectGatesToOr(
      bag_t bag,
      const gate_vector_t<dDNNFGate> &gates,
      const gates_to_or_t &partial);
  [[nodiscard]] gate_vector_t<dDNNFGate> builddDNNFLeaf(bag_t bag);
  [[nodiscard]] gate_vector_t<dDNNFGate> builddDNNF();
  [[nodiscard]] bool circuitHasWire(gate_t u, gate_t v) const;

  [[nodiscard]] bool isAlmostValuation(
    const valuation_t &valuation) const;
  [[nodiscard]] suspicious_t getInnocent(
    const valuation_t &valuation,
    const suspicious_t &innocent) const;

public:
  dDNNFTreeDecompositionBuilder(
      const BooleanCircuit &circuit,
      const BooleanCircuit::uuid &root,
      TreeDecomposition &tree_decomposition) : c{circuit}, td{tree_decomposition}  
  {
    assert(c.hasGate(root));
    root_id = const_cast<BooleanCircuit &>(c).getGate(root);

    for(gate_t i{0}; i<c.getNbGates(); ++i)
      for(auto g: c.getWires(i))
        wiresSet.emplace(i,g);
  };

  [[nodiscard]] dDNNF&& build() &&;
  
  friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};

std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::dDNNFGate &g);

#endif /* dDNNF_TREE_DECOMPOSITION_BUILDER_H  */
