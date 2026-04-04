/**
 * @file MMappedCircuit.h
 * @brief Persistent, mmap-backed storage for the full provenance circuit.
 *
 * @c MMappedCircuit is the authoritative store for all provenance circuit
 * data that must survive transaction boundaries and be accessible across
 * multiple PostgreSQL backends.  It composes three @c MMappedVector
 * instances plus one @c MMappedUUIDHashTable:
 *
 * | Component           | Contents                                      |
 * |---------------------|-----------------------------------------------|
 * | @c mapping          | UUID → gate index (hash table)                |
 * | @c gates            | @c GateInformation records, one per gate      |
 * | @c wires            | Flattened child-UUID lists for all gates      |
 * | @c extra            | Variable-length string data (e.g. provenance labels) |
 *
 * All four backing files live in the PostgreSQL data directory and are
 * opened/created by the ProvSQL background worker at startup.
 *
 * The free-function @c createGenericCircuit() traverses the mmap data
 * starting from a given root UUID to construct an in-memory
 * @c GenericCircuit for evaluation.
 */
#ifndef MMAPPED_CIRCUIT_H
#define MMAPPED_CIRCUIT_H

#include "GenericCircuit.h"
#include "MMappedUUIDHashTable.h"
#include "MMappedVector.hpp"

extern "C" {
#include "provsql_utils.h"
}

/**
 * @brief Per-gate metadata stored in the @c gates @c MMappedVector.
 *
 * Each gate in the persistent circuit has exactly one @c GateInformation
 * record.  The @c children_idx and @c nb_children fields together index
 * into the @c wires @c MMappedVector to find the gate's children.
 * Similarly, @c extra_idx and @c extra_len index into the @c extra vector
 * for variable-length string annotations.
 */
typedef struct GateInformation
{
  gate_type type;            ///< Kind of gate (input, plus, times, …)
  unsigned nb_children;      ///< Number of children
  unsigned long children_idx;///< Start index of this gate's children in @c wires
  double prob;               ///< Associated probability (default 1.0)
  unsigned info1;            ///< General-purpose integer annotation 1
  unsigned info2;            ///< General-purpose integer annotation 2
  unsigned long extra_idx;   ///< Start index in @c extra for string data
  unsigned extra_len;        ///< Byte length of the string data in @c extra

  /**
   * @brief Construct a @c GateInformation with mandatory fields.
   * @param t  Gate type.
   * @param n  Number of children.
   * @param i  Start index of children in the @c wires vector.
   */
  GateInformation(gate_type t, unsigned n, unsigned long i) :
    type(t), nb_children(n), children_idx(i), prob(1.), info1(0), info2(0), extra_idx(0), extra_len(0) {
  }
} GateInformation;

/**
 * @brief Persistent mmap-backed representation of the provenance circuit.
 *
 * @c MMappedCircuit is the single writer for circuit data; only the
 * background worker should call its mutating methods.  Reading methods
 * may be called from any process that has mapped the files read-only.
 */
class MMappedCircuit {
private:
MMappedUUIDHashTable mapping;         ///< UUID → gate-index hash table
MMappedVector<GateInformation> gates; ///< Gate metadata array
MMappedVector<pg_uuid_t> wires;       ///< Flattened child UUID array
MMappedVector<char> extra;            ///< Variable-length string data

static constexpr const char *GATES_FILENAME="provsql_gates.mmap";     ///< Backing file for @c gates
static constexpr const char *WIRES_FILENAME="provsql_wires.mmap";     ///< Backing file for @c wires
static constexpr const char *MAPPING_FILENAME="provsql_mapping.mmap"; ///< Backing file for @c mapping
static constexpr const char *EXTRA_FILENAME="provsql_extra.mmap";     ///< Backing file for @c extra

public:
/**
 * @brief Open all four mmap backing files.
 * @param read_only  If @c true, all files are mapped read-only.
 */
explicit MMappedCircuit(bool read_only = false) :
  mapping(MAPPING_FILENAME, read_only),
  gates(GATES_FILENAME, read_only),
  wires(WIRES_FILENAME, read_only),
  extra(EXTRA_FILENAME, read_only) {
}
/** @brief Sync all backing files before destruction. */
~MMappedCircuit() {
  sync();
}

/**
 * @brief Persist a new gate to the mmap store.
 *
 * Allocates a @c GateInformation record, appends the children to the
 * @c wires vector, and records the UUID→index mapping.  Existing gates
 * with the same @p token are silently skipped.
 *
 * @param token     UUID identifying the new gate.
 * @param type      Gate type.
 * @param children  Ordered list of child gate UUIDs.
 */
void createGate(pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children);

/**
 * @brief Update the @c info1 / @c info2 annotations of a gate.
 * @param token  UUID of the gate to update.
 * @param info1  New value for @c info1.
 * @param info2  New value for @c info2.
 */
void setInfos(pg_uuid_t token, unsigned info1, unsigned info2);

/**
 * @brief Attach a variable-length string annotation to a gate.
 * @param token  UUID of the gate.
 * @param s      String to store.
 */
void setExtra(pg_uuid_t token, const std::string &s);

/**
 * @brief Set the probability associated with a gate.
 * @param token  UUID of the gate.
 * @param prob   Probability value in [0, 1].
 * @return @c true if the gate was found and updated; @c false otherwise.
 */
bool setProb(pg_uuid_t token, double prob);

/**
 * @brief Flush all backing files to disk with @c msync().
 */
void sync();

/**
 * @brief Return the type of the gate identified by @p token.
 * @param token  UUID of the gate.
 * @return       The gate's type, or @c gate_invalid if not found.
 */
gate_type getGateType(pg_uuid_t token) const;

/**
 * @brief Return the child UUIDs of the gate identified by @p token.
 * @param token  UUID of the gate.
 * @return       Ordered vector of child UUIDs.
 */
std::vector<pg_uuid_t> getChildren(pg_uuid_t token) const;

/**
 * @brief Return the probability stored for the gate identified by @p token.
 * @param token  UUID of the gate.
 * @return       The probability, or 1.0 if the gate is not found.
 */
double getProb(pg_uuid_t token) const;

/**
 * @brief Return the @c info1 / @c info2 pair for the gate @p token.
 * @param token  UUID of the gate.
 * @return       Pair @c {info1, info2}, or @c {0,0} if not found.
 */
std::pair<unsigned, unsigned> getInfos(pg_uuid_t token) const;

/**
 * @brief Return the variable-length string annotation for gate @p token.
 * @param token  UUID of the gate.
 * @return       The stored string, or empty if none.
 */
std::string getExtra(pg_uuid_t token) const;

/**
 * @brief Return the total number of gates stored in the circuit.
 * @return Total gate count.
 */
inline unsigned long getNbGates() const {
  return gates.nbElements();
}
};

/**
 * @brief Build an in-memory @c GenericCircuit rooted at @p token.
 *
 * Performs a depth-first traversal of the mmap-backed circuit starting
 * from @p token and copies all reachable gates and wires into a newly
 * constructed @c GenericCircuit.
 *
 * @param token  UUID of the root gate.
 * @return       An in-memory @c GenericCircuit containing the sub-circuit.
 */
GenericCircuit createGenericCircuit(pg_uuid_t token);

#endif /* MMAPPED_CIRCUIT_H */
