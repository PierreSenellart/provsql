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
 * The cache is a pure read accelerator on top of a write-through design:
 * @c create_gate (in @c provsql_mmap.c) populates the cache and
 * unconditionally also forwards the IPC to the background worker, so the
 * worker is always at least as up-to-date as any backend's cache.
 * Evicted entries are therefore simply discarded (no flush hook is
 * needed: the worker already has them). Lookups that miss the cache
 * fall back to an IPC fetch from the worker.
 *
 * Two cache-poisoning hazards motivated the call-site skip rules in
 * @c provsql_mmap.c (search for "Skip caching" there): @c MMappedCircuit
 * returns @c gate_input as a default for unknown tokens, and reports
 * zero children for both real zero-child gates and unknown tokens, so
 * caching such ambiguous results would short-circuit subsequent real
 * @c create_gate calls in the same session. The cache itself is
 * agnostic to that policy.
 */
#ifndef CIRCUIT_CACHE_H
#define CIRCUIT_CACHE_H

#include "provsql_utils.h"

/**
 * @brief Insert a new gate into the circuit cache.
 *
 * Records the gate identified by @p token with the given @p type and
 * @p nb_children children.  If the cache is full the oldest entry is
 * dropped from memory to make room (the worker already holds it under
 * the write-through invariant, so no flush is performed).  If an entry
 * for @p token is already present, its @p type and @p children are
 * overwritten when the incoming call carries strictly more information
 * (a real @c gate_type replacing a previously stored @c gate_invalid,
 * or a non-empty children list replacing an empty one); otherwise the
 * entry is left untouched and just refreshed in LRU order.
 *
 * @param token       UUID identifying the new gate.
 * @param type        Gate type (e.g. @c gate_input, @c gate_plus).
 * @param nb_children Number of child gates.
 * @param children    Array of @p nb_children child UUIDs.
 * @return @c true if the gate was newly inserted, @c false if it was
 *         already present in the cache (regardless of whether its
 *         contents were upgraded).
 */
bool circuit_cache_create_gate(pg_uuid_t token, gate_type type, unsigned nb_children, pg_uuid_t *children);

/**
 * @brief Retrieve the children of a cached gate.
 *
 * Looks up @p token in the cache and, on a hit with at least one
 * child, allocates an array of child UUIDs (via @c calloc) and writes
 * its address to @p *children.
 *
 * @param token     UUID of the gate to look up.
 * @param children  Output parameter: set to a freshly @c calloc'd
 *                  array of child UUIDs on a hit with children; set to
 *                  @c NULL on a miss or on a hit for a zero-child
 *                  entry (the two are deliberately indistinguishable
 *                  to callers, see the file-level note).
 * @return Number of children found, or @c 0 on a miss or zero-child
 *         hit. Callers should detect a miss via @c *children==NULL,
 *         not via the return value.
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
