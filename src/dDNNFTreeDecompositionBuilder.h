#ifndef dDNNF_TREE_DECOMPOSITION_BUILDER_H
#define dDNNF_TREE_DECOMPOSITION_BUILDER_H

#include <cassert>
#include <map>

#include "TreeDecomposition.h"
#include "dDNNF.h"
#include "BooleanCircuit.h"

class dDNNFTreeDecompositionBuilder
{
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
    std::map<gate_t,bool> valuation;
    std::set<gate_t> suspicious;

    dDNNFGate(gate_t i, std::map<gate_t, bool> v, std::set<gate_t> s) :
      id{i}, valuation{std::move(v)}, suspicious{std::move(s)} {}
  };

  std::map<std::pair<std::map<gate_t,bool>,std::set<gate_t>>,std::vector<gate_t>>
    collectGatesToOr(
        const std::vector<dDNNFGate> &gates1,
        const std::vector<dDNNFGate> &gates2,
        bag_t bag);
  std::vector<dDNNFGate> builddDNNFLeaf(bag_t bag);
  std::vector<dDNNFGate> builddDNNF();
  bool circuitHasWire(gate_t u, gate_t v) const;

  bool isAlmostValuation(
    const std::map<gate_t,bool> &valuation) const;
  std::set<gate_t> getSuspicious(
    const std::map<gate_t, bool> &valuation,
    bag_t bag,
    const std::set<gate_t> &ial_innocent) const;

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
