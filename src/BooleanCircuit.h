#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <vector>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.h"

enum class BooleanGate { UNDETERMINED, AND, OR, NOT, IN, MULIN, MULVAR };
class dDNNF;

class BooleanCircuit : public Circuit<BooleanGate> {
private:
bool evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const;
std::string Tseytin(gate_t g, bool display_prob) const;
gate_t interpretAsDDInternal(gate_t g, std::set<gate_t> &seen, dDNNF &dd) const;
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
bool probabilistic=false;

public:
BooleanCircuit() {
}
virtual ~BooleanCircuit() {
}
gate_t addGate() override;
gate_t setGate(BooleanGate t) override;
gate_t setGate(const uuid &u, BooleanGate t) override;
gate_t setGate(BooleanGate t, double p);
gate_t setGate(const uuid &u, BooleanGate t, double p);
const std::set<gate_t> &getInputs() const {
  return inputs;
}
void setProb(gate_t g, double p) {
  if(!probabilistic && p!=1.)
    probabilistic=true;
  prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p;
}
double getProb(gate_t g) const {
  return prob[static_cast<std::underlying_type<gate_t>::type>(g)];
}
bool isProbabilistic() const {
  return probabilistic;
}
void setInfo(gate_t g, unsigned info);
unsigned getInfo(gate_t g) const;

double possibleWorlds(gate_t g) const;
dDNNF compilation(gate_t g, std::string compiler) const;
double monteCarlo(gate_t g, unsigned samples) const;
double WeightMC(gate_t g, std::string opt) const;
double independentEvaluation(gate_t g) const;
void rewriteMultivaluedGates();
dDNNF interpretAsDD(gate_t g) const;
dDNNF makeDD(gate_t g, const std::string &method, const std::string &args) const;

virtual std::string toString(gate_t g) const override;
std::string exportCircuit(gate_t g) const;

template<class Archive>
void serialize (Archive & ar, const unsigned int version)
{
  ar & uuid2id;
  ar & id2uuid;
  ar & gates;
  ar & wires;
  ar & inputs;
  ar & mulinputs;
  ar & prob;
  ar & info;
  ar & probabilistic;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;
};

#endif /* BOOLEAN_CIRCUIT_H */
