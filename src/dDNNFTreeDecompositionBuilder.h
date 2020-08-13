#ifndef dDNNF_TREE_DECOMPOSITION_BUILDER_H
#define dDNNF_TREE_DECOMPOSITION_BUILDER_H

#include <cassert>
#include <map>

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

 private:
  const BooleanCircuit &c;
  BooleanCircuit::uuid root;
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

  std::map<std::pair<valuation_t, suspicious_t>, std::vector<gate_t>>
    collectGatesToOr(
        const std::vector<dDNNFGate> &gates1,
        const std::vector<dDNNFGate> &gates2,
        bag_t bag);
  std::vector<dDNNFGate> builddDNNFLeaf(bag_t bag);
  std::vector<dDNNFGate> builddDNNF();
  bool circuitHasWire(gate_t u, gate_t v) const;

  bool isAlmostValuation(
    const valuation_t &valuation) const;
  suspicious_t getSuspicious(
    const valuation_t &valuation,
    bag_t bag,
    const suspicious_t &innocent) const;

public:
  dDNNFTreeDecompositionBuilder(
      const BooleanCircuit &circuit,
      BooleanCircuit::uuid r,
      TreeDecomposition &tree_decomposition) : c{circuit}, root{std::move(r)}, td{tree_decomposition}  
  {
    assert(c.hasGate(root));
    for(gate_t i{0}; i<c.getNbGates(); ++i)
      for(auto g: c.getWires(i))
        wiresSet.emplace(i,g);
  };

  dDNNF&& build() &&;
  
  friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};

std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::dDNNFGate &g);

#endif /* dDNNF_TREE_DECOMPOSITION_BUILDER_H  */
