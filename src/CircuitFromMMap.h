/**
 * @file CircuitFromMMap.h
 * @brief Build in-memory circuits from the mmap-backed persistent store.
 *
 * Declares two free functions that traverse the memory-mapped circuit
 * storage starting from a given root UUID and construct the corresponding
 * in-memory circuit representation.  These functions are the primary
 * bridge between the persistent @c MMappedCircuit and the evaluation
 * algorithms that operate on @c BooleanCircuit or @c GenericCircuit.
 */
#ifndef BOOLEAN_CIRCUIT_FROM_MMAP_H
#define BOOLEAN_CIRCUIT_FROM_MMAP_H

extern "C" {
#include "provsql_utils.h"
}

#include "BooleanCircuit.h"
#include "GenericCircuit.h"

/**
 * @brief Build a @c BooleanCircuit from the mmap store rooted at @p token.
 *
 * Performs a depth-first traversal of the persistent circuit starting at
 * @p token, translating each @c gate_type to the corresponding
 * @c BooleanGate and copying probabilities and info integers.
 *
 * On return, @p gate is set to the @c gate_t identifier within the
 * returned circuit that corresponds to @p token.
 *
 * @param token  UUID of the root gate.
 * @param gate   Output: @c gate_t of the root within the returned circuit.
 * @return       An in-memory @c BooleanCircuit.
 */
BooleanCircuit getBooleanCircuit(pg_uuid_t token, gate_t &gate);

/**
 * @brief Build a @c BooleanCircuit from an already-loaded @c GenericCircuit.
 *
 * Variant of @c getBooleanCircuit() that reuses an existing
 * @c GenericCircuit (typically obtained via @c getGenericCircuit()) and
 * additionally exposes the @c GenericCircuit-to-@c BooleanCircuit gate
 * mapping, so that callers can translate input-side annotations
 * (e.g., user-supplied leaf labels) from @p gc to the constructed
 * @c BooleanCircuit.
 *
 * @param gc        Source generic circuit (mutated through HAVING
 *                  evaluation).
 * @param token     UUID of the root gate.
 * @param gate      Output: @c gate_t of the root within the returned circuit.
 * @param gc_to_bc  Output: mapping from @c gc input/mulinput gates to the
 *                  corresponding gates in the returned @c BooleanCircuit.
 * @return          An in-memory @c BooleanCircuit.
 */
BooleanCircuit getBooleanCircuit(
  GenericCircuit &gc,
  pg_uuid_t token,
  gate_t &gate,
  std::unordered_map<gate_t, gate_t> &gc_to_bc);

/**
 * @brief Build a @c GenericCircuit from the mmap store rooted at @p token.
 *
 * Equivalent to @c createGenericCircuit() declared in @c MMappedCircuit.h.
 * Performs a depth-first traversal and copies all gate metadata
 * (type, info1/info2, extra strings, probabilities) into the returned
 * @c GenericCircuit.
 *
 * @param token  UUID of the root gate.
 * @return       An in-memory @c GenericCircuit.
 */
GenericCircuit getGenericCircuit(pg_uuid_t token);

/**
 * @brief Build a @c GenericCircuit containing the closures of two
 *        roots, with shared subgraphs unified.
 *
 * Loads the union of every gate reachable from @p root_token and
 * every gate reachable from @p event_token in a single in-memory
 * @c GenericCircuit.  Because @c GenericCircuit's UUID-to-gate
 * mapping is idempotent, a @c gate_rv (or any other gate) reachable
 * from both roots gets exactly one @c gate_t -- the property the
 * conditional MC sampler needs to couple the indicator's draw with
 * the value's draw through @c Sampler's per-iteration cache.
 *
 * The on-load simplification passes that @c getGenericCircuit runs
 * (@c runRangeCheck and @c foldSemiringIdentities, gated by
 * @c provsql.simplify_on_load) are applied to the joint circuit too.
 * Both passes are in-place: gate types may change, but UUID-to-gate_t
 * mappings stay valid, so callers can resolve their two roots via
 * @c getGate after the call returns.
 *
 * Output parameters @p root_gate and @p event_gate are the resolved
 * @c gate_t for the two input UUIDs.
 *
 * @param root_token   First root UUID (e.g. an RV's gate).
 * @param event_token  Second root UUID (e.g. the conditioning gate).
 * @param root_gate    Output: @c gate_t for @p root_token in the
 *                     returned circuit.
 * @param event_gate   Output: @c gate_t for @p event_token in the
 *                     returned circuit.
 * @return             An in-memory @c GenericCircuit.
 */
GenericCircuit getJointCircuit(
  pg_uuid_t root_token,
  pg_uuid_t event_token,
  gate_t &root_gate,
  gate_t &event_gate);

#endif /* BOOLEAN_CIRCUIT_FROM_MMAP_H */
