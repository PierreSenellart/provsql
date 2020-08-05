#ifndef DDNNF_H
#define DDNNF_H

#include "BooleanCircuit.h"

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
  };

  template<unsigned W> 
    std::vector<dDNNFGate> builddDNNF(
        const BooleanCircuit &c, 
        unsigned root,
        const TreeDecomposition<W> &td,
        const std::unordered_map<unsigned, unsigned long> &responsible_bag,
        const std::unordered_map<unsigned, unsigned long> &input_gate,
        const std::unordered_map<unsigned, unsigned long> &negated_input_gate);

 public:
  dDNNF() = default;
  template<unsigned W>
    dDNNF(const BooleanCircuit &c, const uuid &root, TreeDecomposition<W> &td);
  
  double dDNNFEvaluation(unsigned g) const;
    
  friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};
    
std::ostream &operator<<(std::ostream &o, const dDNNF::dDNNFGate &g);

#endif /* DDNNF_H */
