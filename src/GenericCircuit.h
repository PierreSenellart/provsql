#ifndef GENERIC_CIRCUIT_H
#define GENERIC_CIRCUIT_H

#include <map>
#include <type_traits>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.h"
#include "semiring/Semiring.h"

extern "C" {
#include "provsql_utils.h"
}

class GenericCircuit : public Circuit<gate_type>
{
private:
std::map<gate_t, std::pair<unsigned,unsigned> > infos;
std::map<gate_t, std::string> extra;
std::set<gate_t> inputs;

public:
virtual std::string toString(gate_t g) const override {
  return "<GenericCircuit>";
}
void setInfos(gate_t g, unsigned info1, unsigned info2)
{
  infos[g]=std::make_pair(info1, info2);
}
void setExtra(gate_t g, const std::string &ex)
{
  extra[g]=ex;
}
gate_t setGate(gate_type type) override;
gate_t setGate(const uuid &u, gate_type type) override;
const std::set<gate_t> &getInputs() const {
  return inputs;
}

template<class Archive>
void serialize (Archive & ar, const unsigned int version)
{
  ar & uuid2id;
  ar & id2uuid;
  ar & gates;
  ar & wires;
  ar & infos;
  ar & extra;
  ar & inputs;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;

template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> = 0>
typename S::value_type evaluate(gate_t g, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping) const;
};

#endif /* GENERIC_CIRCUIT_H */
