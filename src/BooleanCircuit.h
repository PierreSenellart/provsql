#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "Circuit.hpp"

enum class BooleanGate { UNDETERMINED, AND, OR, NOT, IN };

class BooleanCircuit : public Circuit<BooleanGate> {
 private:
  std::set<unsigned> inputs;
  std::vector<double> prob;
  bool evaluate(unsigned g, const std::unordered_set<unsigned> &sampled) const;
  
 public:
  unsigned addGate() override;
  unsigned setGate(const uuid &u, BooleanGate t) override;
  unsigned setGate(const uuid &u, BooleanGate t, double p);

  double possibleWorlds(unsigned g) const;
  double compilation(unsigned g, std::string compiler) const;
  double monteCarlo(unsigned g, unsigned samples) const;

  double dDNNFEvaluation(unsigned g) const;
  
  virtual std::string toString(unsigned g) const override;
};

#endif /* BOOLEAN_CIRCUIT_H */
