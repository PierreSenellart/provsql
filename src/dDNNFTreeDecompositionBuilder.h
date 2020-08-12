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
  std::unordered_map<unsigned, unsigned long> responsible_bag;
  std::unordered_map<unsigned, unsigned long> input_gate;
  std::unordered_map<unsigned, unsigned long> negated_input_gate;
  
  struct dDNNFGate {
    unsigned long id;
    std::map<unsigned,bool> valuation;
    std::set<unsigned> suspicious;

    dDNNFGate(unsigned long i, std::map<unsigned, bool> v, std::set<unsigned> s) :
      id{i}, valuation{std::move(v)}, suspicious{std::move(s)} {}
  };

  std::map<std::pair<std::map<unsigned,bool>,std::set<unsigned>>,std::vector<unsigned>>
    collectGatesToOr(
        const std::vector<dDNNFGate> &gates1,
        const std::vector<dDNNFGate> &gates2,
        unsigned long gate);
  std::vector<dDNNFGate> builddDNNFLeaf(unsigned gate);
  std::vector<dDNNFGate> builddDNNF(unsigned gate);

  bool isAlmostValuation(
    const std::map<unsigned,bool> &valuation) const;
  std::set<unsigned> getSuspicious(
    const std::map<unsigned, bool> &valuation,
    unsigned long gate,
    const std::set<unsigned> &ial_innocent) const;

public:
  dDNNFTreeDecompositionBuilder(
      const BooleanCircuit &circuit,
      BooleanCircuit::uuid r,
      TreeDecomposition &tree_decomposition) : c{circuit}, root{std::move(r)}, td{tree_decomposition} {
    assert(c.hasGate(root));
  };
  dDNNF build();
  
  friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};

std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::dDNNFGate &g);

#endif /* dDNNF_TREE_DECOMPOSITION_BUILDER_H  */
