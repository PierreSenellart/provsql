/**
 * @file circuit_cache.h
 * @brief C-linkage interface to the in-process provenance circuit cache.
 *
 * The circuit cache is a bounded, LRU-evicting in-memory store that
 * avoids repeated round-trips to the mmap-backed persistent circuit
 * storage for recently accessed gates.  It is implemented in C++ (see
 * @c CircuitCache.h / @c CircuitCache.cpp) but exposed to the C parts of
 * the extension through this header.
 *
 * Gates evicted from the cache are flushed to the background mmap worker
 * via the shared-memory pipe.  Lookups that miss the cache fall back to
 * reading the mmap files directly.
 */
#ifndef CIRCUIT_CACHE_H
#define CIRCUIT_CACHE_H

#include "provsql_utils.h"

/**
 * @brief Insert a new gate into the circuit cache.
 *
 * Records the gate identified by @p token with the given @p type and
 * @p nb_children children.  If the cache is full the oldest entry is
 * evicted (and flushed to persistent storage) to make room.
 *
 * @param token       UUID identifying the new gate.
 * @param type        Gate type (e.g. @c gate_input, @c gate_plus).
 * @param nb_children Number of child gates.
 * @param children    Array of @p nb_children child UUIDs.
 * @return @c true if the gate was inserted, @c false if it was already
 *         present in the cache.
 */
bool circuit_cache_create_gate(pg_uuid_t token, gate_type type, unsigned nb_children, pg_uuid_t *children);

/**
 * @brief Retrieve the children of a cached gate.
 *
 * Looks up @p token in the cache and, on a hit, allocates an array of
 * child UUIDs in the current memory context and writes its address to
 * @p *children.
 *
 * @param token     UUID of the gate to look up.
 * @param children  Output parameter: set to a freshly allocated array of
 *                  child UUIDs on success; untouched on a cache miss.
 * @return Number of children found, or @c UINT_MAX on a cache miss.
 */
unsigned circuit_cache_get_children(pg_uuid_t token, pg_uuid_t **children);

/**
 * @brief Retrieve the type of a cached gate.
 *
 * @param token  UUID of the gate to look up.
 * @return The @c gate_type of the gate, or @c gate_invalid on a miss.
 */
gate_type circuit_cache_get_type(pg_uuid_t token);

#endif /* CIRCUIT_CACHE_H */
