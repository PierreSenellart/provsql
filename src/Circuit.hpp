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
 * - @c getGate(), @c findOrAddGate(): UUID → gate_t lookup, allocating a
 *   new gate on miss.
 * - @c getUUID(): gate_t → UUID string lookup.
 * - @c bindName(), @c pointName(), @c dropName(), @c moveName(): naming
 *   gates.
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
  gate_uuid k;
  if(gate_uuid::parse(u, k))
    return hasGate(k);
  return name2id.find(u)!=name2id.end();
}

template<class gateType>
std::pair<gate_t, bool> Circuit<gateType>::findOrAddGate(const gate_uuid &u)
{
  auto [it, fresh] = uuid2id.try_emplace(u);
  if(fresh) {
    it->second = addGate();
    bindName(it->second, u);
  }
  return {it->second, fresh};
}

template<class gateType>
gate_t Circuit<gateType>::getGate(const uuid &u)
{
  gate_uuid k;
  if(gate_uuid::parse(u, k))
    return getGate(k);
  auto it=name2id.find(u);
  if(it==name2id.end()) {
    gate_t id=addGate();
    bindName(id, u);
    return id;
  } else
    return it->second;
}

template<class gateType>
typename Circuit<gateType>::uuid Circuit<gateType>::getUUID(gate_t g) const
{
  switch(nameKind(g)) {
  case UUID_NAME:
    return id2uuid[static_cast<std::underlying_type<gate_t>::type>(g)].str();
  case OTHER_NAME:
    return id2name.at(g);
  default:
    return "";
  }
}

template<class gateType>
void Circuit<gateType>::bindName(gate_t g, const gate_uuid &u)
{
  const auto i = static_cast<std::underlying_type<gate_t>::type>(g);
  if(i >= name_kind.size()) {
    name_kind.resize(i + 1, NO_NAME);
    id2uuid.resize(i + 1);
  }
  if(name_kind[i] == OTHER_NAME)
    id2name.erase(g);
  uuid2id[u] = g;
  id2uuid[i] = u;
  name_kind[i] = UUID_NAME;
}

template<class gateType>
void Circuit<gateType>::bindName(gate_t g, const uuid &u)
{
  gate_uuid k;
  if(gate_uuid::parse(u, k)) {
    bindName(g, k);
    return;
  }
  const auto i = static_cast<std::underlying_type<gate_t>::type>(g);
  if(i >= name_kind.size()) {
    name_kind.resize(i + 1, NO_NAME);
    id2uuid.resize(i + 1);
  }
  name2id[u] = g;
  id2name[g] = u;
  name_kind[i] = OTHER_NAME;
}

template<class gateType>
void Circuit<gateType>::pointName(gate_t g, gate_t target)
{
  const auto i = static_cast<std::underlying_type<gate_t>::type>(g);
  switch(nameKind(g)) {
  case UUID_NAME:
    uuid2id[id2uuid[i]] = target;
    break;
  case OTHER_NAME:
    name2id[id2name[g]] = target;
    break;
  default:
    break;
  }
}

template<class gateType>
void Circuit<gateType>::dropName(gate_t g)
{
  const auto i = static_cast<std::underlying_type<gate_t>::type>(g);
  switch(nameKind(g)) {
  case UUID_NAME:
    uuid2id.erase(id2uuid[i]);
    break;
  case OTHER_NAME:
    name2id.erase(id2name[g]);
    id2name.erase(g);
    break;
  default:
    return;
  }
  name_kind[i] = NO_NAME;
}

template<class gateType>
void Circuit<gateType>::moveName(gate_t from, gate_t to)
{
  const auto i = static_cast<std::underlying_type<gate_t>::type>(from);
  switch(nameKind(from)) {
  case UUID_NAME: {
    const gate_uuid u = id2uuid[i];
    name_kind[i] = NO_NAME;
    bindName(to, u);
    break;
  }
  case OTHER_NAME: {
    const std::string u = id2name[from];
    id2name.erase(from);
    name_kind[i] = NO_NAME;
    bindName(to, u);
    break;
  }
  default:
    break;
  }
}

template<class gateType>
gate_t Circuit<gateType>::addGate()
{
  gate_t id{gates.size()};
  gates.push_back(gateType());
  wires.push_back({});
  id2uuid.emplace_back();
  name_kind.push_back(NO_NAME);
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
