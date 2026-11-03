#ifndef CIRCUIT_CANONICAL_FORM_H
#define CIRCUIT_CANONICAL_FORM_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "BooleanCircuit.h"

namespace provsql {

struct CanonicalGateHash {
  size_t operator()(gate_t g) const {
    return std::hash<typename std::underlying_type<gate_t>::type>()(
      static_cast<typename std::underlying_type<gate_t>::type>(g));
  }
};

using GateSet = std::unordered_set<gate_t, CanonicalGateHash>;

struct CanonicalLeaf {
  gate_t gate;
  bool is_abstract;
};

struct CanonicalForm {
  std::string form;
  uint64_t hash;
  std::vector<CanonicalLeaf> leaves;
};

GateSet reachableNodes(const BooleanCircuit &c, gate_t root,
                       const GateSet *abstract_leaves = nullptr);

CanonicalForm computeCanonicalForm(const BooleanCircuit &c, gate_t root,
                                   const GateSet *abstract_leaves = nullptr);

std::vector<gate_t> maximalIslandRoots(const BooleanCircuit &c, gate_t root);

struct IslandKey {
  CanonicalForm residual;
  std::vector<gate_t> island_roots;
  std::vector<CanonicalForm> islands;
};

IslandKey computeIslandKey(const BooleanCircuit &c, gate_t root);

uint64_t fnv1aHash(const std::string &s);

}

#endif
