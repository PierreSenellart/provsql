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

#endif /* BOOLEAN_CIRCUIT_FROM_MMAP_H */
