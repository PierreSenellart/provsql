/**
 * @file CircuitCanonicalForm.h
 * @brief Canonical form of a Boolean circuit, and its split into islands.
 *
 * The canonical form of a circuit is a string that spells out its
 * structure and forgets which inputs it is over: an input reads @c V the
 * first time it is met and a gate met again reads @c \#k, its rank among
 * the nodes of the form.  The children of an AND or an OR are ordered by
 * a colour computed bottom-up from the type, the fan-out and the colours
 * of the children, so that the order they are stored in does not matter.
 *
 * Two circuits of equal form have the same structure, with the inputs of
 * one matching those of the other in the order of
 * @c CanonicalForm::leaves: a d-DNNF compiled for one evaluates the other
 * once the probabilities are replaced.  The converse does not hold: when
 * children are tied on colour their stored order decides, so two
 * isomorphic circuits may get different forms.  For the cache of
 * @c ProbCache.h this only costs a compilation.
 *
 * An *island* is the part of the circuit below a gate that the rest of
 * the circuit reaches only through that gate: it shares no input with
 * the rest, so its probability can be computed on its own and the gate
 * treated as an independent input.  @c computeIslandKey splits a circuit
 * into its outermost islands and the *residual*, the circuit with each
 * island replaced by an input.  (These are not the certified regions
 * that the island discipline of @c independentEvaluation refers to.)
 */
#ifndef CIRCUIT_CANONICAL_FORM_H
#define CIRCUIT_CANONICAL_FORM_H

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "BooleanCircuit.h"

namespace provsql {

/// Hash of a @c gate_t, for the containers of this module.
struct CanonicalGateHash {
  size_t operator()(gate_t g) const {
    return std::hash<typename std::underlying_type<gate_t>::type>()(
      static_cast<typename std::underlying_type<gate_t>::type>(g));
  }
};

/// Set of gates of one circuit.
using GateSet = std::unordered_set<gate_t, CanonicalGateHash>;

/// A leaf of a canonical form, in the order the form meets it.
struct CanonicalLeaf {
  gate_t gate;       ///< The gate of the circuit this leaf stands for
  bool is_abstract;  ///< Whether it is an island root read as an input
};

/// Canonical form of a circuit below a root.
struct CanonicalForm {
  std::string form;                   ///< The form, e.g. `(OR (AND V V) (AND V #4))`
  uint64_t hash;                      ///< FNV-1a hash of @c form
  std::vector<CanonicalLeaf> leaves;  ///< The leaves, in the order of their @c V in @c form
};

/**
 * @brief Gates reachable from @p root.
 * @param c               The circuit.
 * @param root            Gate to start from.
 * @param abstract_leaves Gates not to go below (other than @p root); they
 *                        are part of the result.
 */
GateSet reachableNodes(const BooleanCircuit &c, gate_t root,
                       const GateSet *abstract_leaves = nullptr);

/**
 * @brief Canonical form of the circuit below @p root.
 * @param c               The circuit.
 * @param root            Root of the form.
 * @param abstract_leaves Gates read as inputs, whatever is below them.
 *
 * Iterative, so that a circuit as deep as the data does not exhaust the
 * stack.
 */
CanonicalForm computeCanonicalForm(const BooleanCircuit &c, gate_t root,
                                   const GateSet *abstract_leaves = nullptr);

/**
 * @brief Roots of the outermost islands below @p root.
 *
 * An AND, OR or NOT gate other than @p root is an island root when no
 * gate below it is reached from @p root by a path avoiding it.  Only the
 * outermost ones are returned, those not inside another island.  One
 * depth-first traversal, linear in the size of the circuit.
 */
std::vector<gate_t> maximalIslandRoots(const BooleanCircuit &c, gate_t root);

/// A circuit split into its outermost islands and what remains.
struct IslandKey {
  CanonicalForm residual;              ///< Form of the circuit, each island read as an input
  std::vector<gate_t> island_roots;    ///< Island roots, in the order of the residual's leaves
  std::vector<CanonicalForm> islands;  ///< Form of each island, in the same order
};

/// Splits the circuit below @p root into its outermost islands and the residual.
IslandKey computeIslandKey(const BooleanCircuit &c, gate_t root);

/// 64-bit FNV-1a hash of @p s.
uint64_t fnv1aHash(const std::string &s);

}

#endif
