/**
 * @file GenericCircuit.cpp
 * @brief GenericCircuit method implementations.
 *
 * Implements the virtual methods of @c GenericCircuit that override the
 * @c Circuit<gate_type> base class:
 * - @c addGate(): allocates a new gate and extends the @c prob vector.
 * - @c setGate(gate_type): creates a new gate, registering it as an
 *   input gate when the type is @c gate_input or @c gate_update.
 * - @c setGate(const uuid&, gate_type): same with UUID binding.
 *
 * The template method @c evaluate() is defined in @c GenericCircuit.hpp.
 */
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
