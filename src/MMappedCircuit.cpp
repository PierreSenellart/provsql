#include "MMappedCircuit.h"

extern "C" {
#include "provsql_mmap.h"
}

static MMappedCircuit *c;

void initialize_provsql_mmap()
{
  c = new MMappedCircuit();
}

void destroy_provsql_mmap()
{
  delete c;
}

void MMappedCircuit::createGate(
  pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children)
{
  auto [idx, created] = mapping.add(token);
  if(!created) // Was already existing, no need to do anything
    return;

  gates.add({type, static_cast<unsigned>(children.size()), wires.nbElements()});
  for(const auto &c: children)
  {
    wires.add(c);
  }
}

gate_type MMappedCircuit::getGateType(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return gate_invalid;
  else
    return gates[idx].type;
}

std::vector<pg_uuid_t> MMappedCircuit::getChildren(pg_uuid_t token) const
{
  std::vector<pg_uuid_t> result;
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    const GateInformation &gi = gates[idx];
    for(unsigned k=gi.children_idx; k<gi.children_idx+gi.nb_children; ++k)
      result.push_back(wires[k]);
  }
  return result;
}

void MMappedCircuit::setProb(pg_uuid_t token, double prob)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING)
    gates[idx].prob=prob;
}

double MMappedCircuit::getProb(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return -1;
  else
    return gates[idx].prob;
}

void MMappedCircuit::setInfos(pg_uuid_t token, unsigned info1, unsigned info2)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    gates[idx].info1=info1;
    gates[idx].info2=info2;
  }
}

std::pair<unsigned, unsigned> MMappedCircuit::getInfos(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING) {
    return std::make_pair(0, 0);
  } else {
    const GateInformation &gi = gates[idx];
    return std::make_pair(gi.info1, gi.info2);
  }
}
