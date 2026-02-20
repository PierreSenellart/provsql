#include "GenericCircuit.h"

gate_t GenericCircuit::setGate(gate_type type)
{
  auto id = Circuit::setGate(type);
  if(type == gate_input || type==gate_update) {
    inputs.insert(id);
  }
  return id;
}

gate_t GenericCircuit::setGate(const uuid &u, gate_type type)
{
  auto id = Circuit::setGate(u, type);
  if(type == gate_input || type==gate_update) {
    inputs.insert(id);
  }
  return id;
}

gate_t GenericCircuit::addGate()
{
  auto id=Circuit::addGate();
  prob.push_back(1);
  return id;
}
