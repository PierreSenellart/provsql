#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <vector>

#include "Circuit.hpp"

enum class BooleanGate { UNDETERMINED, AND, OR, NOT, IN, MULIN, MULVAR };

class BooleanCircuit : public Circuit<BooleanGate> {
 private:
  bool evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const;
  std::string Tseytin(gate_t g, bool display_prob) const;
  double independentEvaluationInternal(gate_t g, std::set<gate_t> &seen) const;
  void rewriteMultivaluedGatesRec(
    const std::vector<gate_t> &muls,
    const std::vector<double> &cumulated_probs,
    unsigned start,
    unsigned end,
    std::vector<gate_t> &prefix);

 protected:
  std::set<gate_t> inputs;
  std::set<gate_t> mulinputs;
  std::vector<double> prob;
  std::map<gate_t, unsigned> info;

 public:
  gate_t addGate() override;
  gate_t setGate(BooleanGate t) override;
  gate_t setGate(const uuid &u, BooleanGate t) override;
  gate_t setGate(BooleanGate t, double p);
  gate_t setGate(const uuid &u, BooleanGate t, double p);
  void setProb(gate_t g, double p) { prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p; }
  double getProb(gate_t g) const { return prob[static_cast<std::underlying_type<gate_t>::type>(g)]; }
  void setInfo(gate_t g, unsigned info);
  unsigned getInfo(gate_t g) const;

  double possibleWorlds(gate_t g) const;
  double compilation(gate_t g, std::string compiler) const;
  double monteCarlo(gate_t g, unsigned samples) const;
  double WeightMC(gate_t g, std::string opt) const;
  double independentEvaluation(gate_t g) const;
  void rewriteMultivaluedGates();

  virtual std::string toString(gate_t g) const override;

  friend class dDNNFTreeDecompositionBuilder;
};

#endif /* BOOLEAN_CIRCUIT_H */
