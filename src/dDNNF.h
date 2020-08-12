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

 public:
  double dDNNFEvaluation(unsigned g) const;
    
  friend dDNNFTreeDecompositionBuilder;
  friend double BooleanCircuit::compilation(unsigned g, std::string compiler) const;
};
    
#endif /* DDNNF_H */
