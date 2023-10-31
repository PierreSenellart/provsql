#ifndef DDNNF_H
#define DDNNF_H

#include <string>

#include "BooleanCircuit.h"

// Forward declaration for friend
class dDNNFTreeDecompositionBuilder;

struct hash_gate_t
{
  size_t operator()(gate_t g) const
  {
    return std::hash<typename std::underlying_type<gate_t>::type>()(
      static_cast<typename std::underlying_type<gate_t>::type>(g));
  }
};

class dDNNF : public BooleanCircuit {
private:
dDNNF() = default;

// To memoize results
mutable std::unordered_map<gate_t, double, hash_gate_t> cache;

public:
double dDNNFEvaluation(gate_t g) const;

friend dDNNFTreeDecompositionBuilder;
friend double BooleanCircuit::compilation(gate_t g, std::string compiler) const;
};

#endif /* DDNNF_H */
