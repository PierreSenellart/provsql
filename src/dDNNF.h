#ifndef DDNNF_H
#define DDNNF_H

#include <string>
#include <unordered_set>

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
// To memoize probability evaluation results
mutable std::unordered_map<gate_t, double, hash_gate_t> probability_cache;
std::unordered_map<gate_t, std::vector<double> > shapley_delta(gate_t root) const;
std::vector<std::vector<double> > shapley_alpha(gate_t root) const;
std::vector<gate_t> topological_order(const std::vector<std::vector<gate_t> > &reversedWires) const;
gate_t root;

public:
gate_t getRoot() const {
  return root;
}
std::unordered_set<gate_t> vars(gate_t root) const;
void makeSmooth();
void makeAndGatesBinary();
void simplify();
dDNNF conditionAndSimplify(gate_t var, bool value) const;
dDNNF condition(gate_t var, bool value) const;
double dDNNFProbabilityEvaluation(gate_t root) const;
double shapley(gate_t g, gate_t var) const;

friend dDNNFTreeDecompositionBuilder;
friend double BooleanCircuit::compilation(gate_t g, std::string compiler) const;
};

#endif /* DDNNF_H */
