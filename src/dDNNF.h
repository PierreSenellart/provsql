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
std::unordered_map<gate_t, std::vector<double> > shapley_delta() const;
std::vector<std::vector<double> > shapley_alpha() const;
double banzhaf_internal() const;
std::vector<gate_t> topological_order(const std::vector<std::vector<gate_t> > &reversedWires) const;
gate_t root{0};

public:
gate_t getRoot() const {
  return root;
}
void setRoot(gate_t g) {
  root=g;
}
std::unordered_set<gate_t> vars(gate_t root) const;
void makeSmooth();
void makeGatesBinary(BooleanGate type);
void simplify();
dDNNF conditionAndSimplify(gate_t var, bool value) const;
dDNNF condition(gate_t var, bool value) const;
double probabilityEvaluation() const;
double shapley(gate_t var) const;
double banzhaf(gate_t var) const;

friend dDNNFTreeDecompositionBuilder;
friend dDNNF BooleanCircuit::compilation(gate_t g, std::string compiler) const;
};

#endif /* DDNNF_H */
