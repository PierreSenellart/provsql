#include "Circuit.h"

template<class gateType>
bool Circuit<gateType>::hasGate(const uuid &u) const
{
  return uuid2id.find(u)!=uuid2id.end();
}

template<class gateType>
unsigned Circuit<gateType>::getGate(const uuid &u)
{
  auto it=uuid2id.find(u);
  if(it==uuid2id.end()) {
    unsigned id=addGate();
    uuid2id[u]=id;
    return id;
  } else 
    return it->second;
}

template<class gateType>
unsigned Circuit<gateType>::addGate()
{
  unsigned id=gates.size();
  gates.push_back(gateType());
  wires.resize(id+1);
  return id;
}

template<class gateType>
unsigned Circuit<gateType>::setGate(const uuid &u, gateType type)
{
  unsigned id = getGate(u);
  gates[id] = type;
  return id;
}

template<class gateType>
void Circuit<gateType>::addWire(unsigned f, unsigned t)
{
  wires[f].push_back(t);
}
