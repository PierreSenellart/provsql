/**
 * @file CertifiedDDMaterialize.h
 * @brief Content-addressed materialisation of a certified d-D into the
 *        mmap provenance store.
 *
 * A decomposition-aligned compiler (the reachability compiler, the
 * joint-width UCQ compiler) builds a circuit that is **deterministic and
 * decomposable by construction** -- a certified d-D.  Rather than reading
 * the probability off that circuit in C++, the certified d-D can be
 * *materialised* as ordinary provenance gates (@c plus / @c times /
 * @c monus, carrying the @c DNNF_CERT_INFO mark), so that the answer --
 * probability, Shapley, expectation, any provenance-store evaluation --
 * is obtained through the **single** standard entry point
 * @c probability_evaluate() (etc.) on the materialised token.  This keeps
 * one evaluation path for the whole system and exploits the certificate
 * (linear-time @c independentEvaluation / @c interpretAsDD).
 *
 * Gate UUIDs are content-addressed with the standard v5 recipes, so
 * identical sub-circuits dedup in the store and re-materialising the same
 * circuit is a no-op.
 */
#ifndef CERTIFIED_DD_MATERIALIZE_H
#define CERTIFIED_DD_MATERIALIZE_H

extern "C" {
#include "postgres.h"
#include "utils/uuid.h"
}

#include "dDNNF.h"

#include <unordered_map>
#include <vector>

/**
 * @brief RFC 4122 version-5 UUID in the ProvSQL namespace.
 *
 * Same value as the SQL @c uuid_generate_v5(uuid_ns_provsql(), name), so
 * gates content-address identically to those the query rewriter mints.
 *
 * @param name  Name within the namespace.
 * @return      The version-5 UUID.
 */
pg_uuid_t provsqlUuidV5(const std::string &name);

/**
 * @brief Materialise (the reachable part of) a certified d-D into the
 *        mmap store.
 *
 * Bottom-up over the gates reachable from @p roots: input gates keep
 * their existing tokens (the leaf provenance, not touched), @c NOT @c x
 * is stored as @c monus(one, x), AND as @c times, OR as @c plus.  Gates
 * carrying the d-D certificate get it persisted.
 *
 * @param dd     The certified circuit.
 * @param roots  Gates whose closure to materialise.
 * @return       Map from @p dd gates to their store UUIDs.
 */
std::unordered_map<gate_t, pg_uuid_t, hash_gate_t> materializeCertifiedDD(
  const dDNNF &dd, const std::vector<gate_t> &roots);

/**
 * @brief Wrap a materialised root in the @c 'absorptive' assumption
 *        marker and return the wrapper's UUID.
 *
 * Used by the reachability route, whose compiled circuit is the
 * absorptive quotient of a genuinely infinite recursive provenance.  The
 * joint-width UCQ compiler does NOT use this: its d-D is the exact
 * Boolean provenance of the (non-recursive) UCQ, sound for every
 * absorptive-or-not probability evaluation.
 *
 * @param child  UUID of the materialised root to wrap.
 * @return       UUID of the @c gate_assumed wrapper.
 */
pg_uuid_t wrapAssumedAbsorptive(const pg_uuid_t &child);

#endif /* CERTIFIED_DD_MATERIALIZE_H */
