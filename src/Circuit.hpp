#include "Circuit.h"

template<class gateType>
bool Circuit<gateType>::hasGate(const uuid &u) const
{
  return uuid2id.find(u)!=uuid2id.end();
}

template<class gateType>
gate_t Circuit<gateType>::getGate(const uuid &u)
{
  auto it=uuid2id.find(u);
  if(it==uuid2id.end()) {
    gate_t id=addGate();
    uuid2id[u]=id;
    return id;
  } else 
    return it->second;
}

template<class gateType>
gate_t Circuit<gateType>::addGate()
{
  gate_t id{gates.size()};
  gates.push_back(gateType());
  wires.push_back({});
  return id;
}

template<class gateType>
gate_t Circuit<gateType>::setGate(gateType type)
{
  gate_t id = addGate();
  gates[static_cast<std::underlying_type<gate_t>::type>(id)] = type;
  return id;
}

template<class gateType>
gate_t Circuit<gateType>::setGate(const uuid &u, gateType type)
{
  gate_t id = getGate(u);
  gates[static_cast<std::underlying_type<gate_t>::type>(id)] = type;
  return id;
}

template<class gateType>
void Circuit<gateType>::addWire(gate_t f, gate_t t)
{
  getWires(f).push_back(t);
}
