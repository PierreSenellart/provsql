#ifndef DDNNF_H
#define DDNNF_H

#include <iostream>
#include <string>

#include "BooleanCircuit.h"

// Forward declaration for friend
class dDNNFTreeDecompositionBuilder;

class dDNNF : public BooleanCircuit {
 private:
  dDNNF() = default;

  // To memoize results
  mutable std::unordered_map<gate_t, double> cache;

 public:
  double dDNNFEvaluation(gate_t g) const;
    
  friend dDNNFTreeDecompositionBuilder;
  friend double BooleanCircuit::compilation(gate_t g, std::string compiler) const;
};
    
#endif /* DDNNF_H */
