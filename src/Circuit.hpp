/**
 * @file Circuit.hpp
 * @brief Out-of-line template method implementations for @c Circuit<gateType>.
 *
 * This file provides the definitions of the template methods declared in
 * @c Circuit.h that cannot be placed in a @c .cpp file (since they must
 * be instantiated by the compiler in each translation unit that uses them).
 *
 * Implemented methods:
 * - @c hasGate(): UUID membership test.
 * - @c getGate(): UUID → gate_t lookup, allocating a new gate on miss.
 * - @c getUUID(): gate_t → UUID string lookup.
 * - @c addGate(): default gate allocation (extends @c gates and @c wires vectors).
 * - @c setGate(gateType): allocate a new gate and set its type.
 * - @c setGate(const uuid&, gateType): create/update a UUID-named gate.
 * - @c addWire(): append a directed edge.
 *
 * Included by subclass headers that need these implementations (e.g.
 * @c BooleanCircuit.h, @c DotCircuit.h).
 */
#ifndef CIRCUIT_HPP
#define CIRCUIT_HPP

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
    id2uuid[id]=u;
    return id;
  } else
    return it->second;
}

template<class gateType>
typename Circuit<gateType>::uuid Circuit<gateType>::getUUID(gate_t g) const
{
  auto it = id2uuid.find(g);
  if(it==id2uuid.end())
    return "";
  else
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

#endif /* CIRCUIT_HPP */
