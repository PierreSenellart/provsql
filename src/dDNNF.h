#ifndef DDNNF_H
#define DDNNF_H

#include "BooleanCircuit.h"

#include <iostream>
#include <map>

// Forward definition of TreeDecomposition, as TreeDecomposition also
// needs to refer to the dDNNF class
template<unsigned W> class TreeDecomposition;

class dDNNF : public BooleanCircuit {
 private:
  struct dDNNFGate {
    unsigned long id;
    std::map<unsigned,bool> valuation;
    std::set<unsigned> suspicious;

    dDNNFGate(unsigned long i, std::map<unsigned, bool> v, std::set<unsigned> s) :
      id{i}, valuation{std::move(v)}, suspicious{std::move(s)} {}
  };


  template<unsigned W>
    std::map<std::pair<std::map<unsigned,bool>,std::set<unsigned>>,std::vector<unsigned>>
    collectGatesToOr(
        const std::vector<dDNNFGate> &gates1,
        const std::vector<dDNNFGate> &gates2,
        const BooleanCircuit &c, 
        unsigned long root,
        const TreeDecomposition<W> &td);
  template<unsigned W> 
  std::vector<dDNNF::dDNNFGate> builddDNNFLeaf(
      const BooleanCircuit &c, 
      unsigned root,
      const TreeDecomposition<W> &td,
      const std::unordered_map<unsigned, unsigned long> &responsible_bag,
      const std::unordered_map<unsigned, unsigned long> &input_gate,
      const std::unordered_map<unsigned, unsigned long> &negated_input_gate);
  template<unsigned W> 
    std::vector<dDNNFGate> builddDNNF(
        const BooleanCircuit &c, 
        unsigned root,
        const TreeDecomposition<W> &td,
        const std::unordered_map<unsigned, unsigned long> &responsible_bag,
        const std::unordered_map<unsigned, unsigned long> &input_gate,
        const std::unordered_map<unsigned, unsigned long> &negated_input_gate);

  static bool isAlmostValuation(
    const std::map<unsigned,bool> &valuation,
    const BooleanCircuit &c);
  template<unsigned W>
  static std::set<unsigned> getSuspicious(
    const std::map<unsigned, bool> &valuation,
    const BooleanCircuit &c,
    unsigned root,
    const TreeDecomposition<W> &td,
    const std::set<unsigned> &partial_innocent);

 public:
  dDNNF() = default;
  template<unsigned W>
    dDNNF(const BooleanCircuit &c, const uuid &root, TreeDecomposition<W> &td);
  
  double dDNNFEvaluation(unsigned g) const;
    
  friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};
    
std::ostream &operator<<(std::ostream &o, const dDNNF::dDNNFGate &g);

#endif /* DDNNF_H */
