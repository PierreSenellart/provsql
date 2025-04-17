#ifndef GENERIC_CIRCUIT_H
#define GENERIC_CIRCUIT_H

#include <map>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.h"

extern "C" {
#include "provsql_utils.h"
}

class GenericCircuit : public Circuit<gate_type>
{
private:
std::map<gate_t, std::pair<unsigned,unsigned> > infos;
std::map<gate_t, std::string> extra;

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

template<class Archive>
void serialize (Archive & ar, const unsigned int version)
{
  ar & uuid2id;
  ar & id2uuid;
  ar & gates;
  ar & wires;
  ar & infos;
  ar & extra;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;
};

#endif /* GENERIC_CIRCUIT_H */
