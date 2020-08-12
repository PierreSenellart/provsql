#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "Circuit.hpp"

enum class BooleanGate { UNDETERMINED, AND, OR, NOT, IN };

class dDNNF;

class BooleanCircuit : public Circuit<BooleanGate> {
 private:
  bool evaluate(unsigned g, const std::unordered_set<unsigned> &sampled) const;
  std::string Tseytin(unsigned g, bool display_prob) const;

 protected:
  std::set<unsigned> inputs;
  std::vector<double> prob;

 public:
  unsigned addGate() override;
  unsigned setGate(BooleanGate t) override;
  unsigned setGate(const uuid &u, BooleanGate t) override;
  unsigned setGate(BooleanGate t, double p);
  unsigned setGate(const uuid &u, BooleanGate t, double p);

  double possibleWorlds(unsigned g) const;
  double compilation(unsigned g, std::string compiler) const;
  double monteCarlo(unsigned g, unsigned samples) const;
  double WeightMC(unsigned g, std::string opt) const;

  virtual std::string toString(unsigned g) const override;

  // We make dDNNF a friend of BooleanCircuit, since its constructor
  // needs to manipulate the internals of a BooleanCircuit
  friend class dDNNFTreeDecompositionBuilder;
};

#endif /* BOOLEAN_CIRCUIT_H */
