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
  bool evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const;
  std::string Tseytin(gate_t g, bool display_prob) const;
  double independentEvaluationInternal(gate_t g, std::set<gate_t> &seen) const;

 protected:
  std::set<gate_t> inputs;
  std::vector<double> prob;

 public:
  gate_t addGate() override;
  gate_t setGate(BooleanGate t) override;
  gate_t setGate(const uuid &u, BooleanGate t) override;
  gate_t setGate(BooleanGate t, double p);
  gate_t setGate(const uuid &u, BooleanGate t, double p);
  void setProb(gate_t g, double p) { prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p; }
  double getProb(gate_t g) const { return prob[static_cast<std::underlying_type<gate_t>::type>(g)]; }

  double possibleWorlds(gate_t g) const;
  double compilation(gate_t g, std::string compiler) const;
  double monteCarlo(gate_t g, unsigned samples) const;
  double WeightMC(gate_t g, std::string opt) const;
  double independentEvaluation(gate_t g) const;

  virtual std::string toString(gate_t g) const override;

  friend class dDNNFTreeDecompositionBuilder;
};

#endif /* BOOLEAN_CIRCUIT_H */
