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
 * All four backing files live in the database's directory inside the
 * PostgreSQL data directory, and are opened/created by the ProvSQL
 * background worker on the first message for that database.
 *
 * The free-function @c createGenericCircuit() traverses the mmap data
 * starting from a given root UUID to construct an in-memory
 * @c GenericCircuit for evaluation.
 *
 * @warning ON-DISK ABI: the layouts of @c GateInformation, of the
 * @c gate_type enum (defined in @c provsql_utils.h), of @c pg_uuid_t,
 * and of @c MMappedUUIDHashTable's slot structure are all serialised
 * verbatim into the four @c provsql_*.mmap backing files.  ProvSQL
 * supports in-place extension upgrades (@c ALTER @c EXTENSION @c provsql
 * @c UPDATE) only because these layouts have been stable since
 * ProvSQL 1.0.0.  Any change that adds, removes, reorders, or resizes
 * a field -- or that renumbers a @c gate_type enumerator -- silently
 * breaks every existing installation's on-disk mmap files.  If such a
 * change is necessary, bump an explicit format-version header in the
 * mmap files, write a migration path, and call it out in a release note.
 */
#ifndef MMAPPED_CIRCUIT_H
#define MMAPPED_CIRCUIT_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

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
  gate_type type;            ///< Kind of gate (input, plus, times…)
  unsigned nb_children;      ///< Number of children
  unsigned long children_idx;///< Start index of this gate's children in @c wires
  double prob;               ///< Associated probability, @c NaN when unset
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
    type(t), nb_children(n), children_idx(i), prob(NAN), info1(0), info2(0), extra_idx(0), extra_len(0) {
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

/** @brief Append a complete gate record, then publish @p token for it. */
void appendGate(pg_uuid_t token, gate_type type,
                const std::vector<pg_uuid_t> &children);

/** @brief Whether the gate record at index @p idx holds a written
 *  probability (see @c GATES_VERSION for the version-1 leniency). */
bool hasProbAt(unsigned long idx) const;



/** @brief Delegating constructor that accepts pre-built paths. */
MMappedCircuit(const std::string &mp, const std::string &gp,
               const std::string &wp, const std::string &ep,
               bool read_only) :
  mapping(mp.c_str(), read_only, MAGIC_MAPPING),
  gates  (gp.c_str(), read_only, MAGIC_GATES, GATES_VERSION),
  wires  (wp.c_str(), read_only, MAGIC_WIRES),
  extra  (ep.c_str(), read_only, MAGIC_EXTRA) {}

public:
/**
 * @brief Format version of the @c gates file this build writes.
 *
 * Version 1 stored @c 1.0 in @c GateInformation::prob both for a gate
 * whose probability had been set to 1 and for one that had never been
 * given a probability at all.  Version 2 writes @c NaN for the latter,
 * which is what lets a probability be written once and refused
 * afterwards.  A version-1 file is still read: @c 1.0 on a
 * probability-bearing gate is then treated as unset, so a store
 * carried across an upgrade keeps accepting the probabilities its
 * owner has always been allowed to write.  The ambiguity lasts until
 * @c circuit_cleanup rewrites the file, which normalises those
 * @c 1.0 values and stamps the file version 2.
 */
static constexpr uint16_t GATES_VERSION = 2;

/**
 * @brief Outcome of @c setProb.
 *
 * @c Written and @c Unchanged are both successes; only @c Written
 * needs undoing if the writing transaction rolls back.
 */
enum class SetProbResult {
  NotProbGate,  ///< The token names a gate that carries no probability
  Written,      ///< The gate had no probability and now holds the given one
  Unchanged,    ///< The gate already held exactly this probability
  AlreadySet    ///< The gate holds a different probability; refused
};


/** @brief Build the full path of a file in a database's store directory,
 *  the one @c GetDatabasePath resolves for @p db_oid in @p db_tablespace. */
static std::string storePath(Oid db_oid, Oid db_tablespace,
                             const char *filename);

/**
 * @brief Outcome of @c setInfos or @c setExtra.
 *
 * Same discipline as @c setProb: an annotation is a fact appended to the
 * gate, written once and idempotent on the same value.
 */
enum class SetAnnotationResult {
  NoSuchGate,   ///< The token names no gate
  Written,      ///< The gate had none and now holds the given annotation
  Unchanged,    ///< The gate already held exactly this annotation
  AlreadySet    ///< The gate holds a different annotation; refused
};

/** @brief 8-byte magic constants identifying each mmap file type. */
static constexpr uint64_t MAGIC_GATES =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('G') << 24 |
  uint64_t('a') << 32 | uint64_t('t') << 40 | uint64_t('e') << 48 | uint64_t('s') << 56;
static constexpr uint64_t MAGIC_WIRES =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('W') << 24 |
  uint64_t('i') << 32 | uint64_t('r') << 40 | uint64_t('e') << 48 | uint64_t('s') << 56;
static constexpr uint64_t MAGIC_MAPPING =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('M') << 24 |
  uint64_t('a') << 32 | uint64_t('p') << 40 | uint64_t('n') << 48 | uint64_t('g') << 56;
static constexpr uint64_t MAGIC_EXTRA =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('E') << 24 |
  uint64_t('x') << 32 | uint64_t('t') << 40 | uint64_t('r') << 48 | uint64_t('a') << 56;

/**
 * @brief Open all four mmap backing files for the given database.
 * @param db_oid        OID of the target database.
 * @param db_tablespace OID of that database's default tablespace; the
 *                      files go in the directory @c GetDatabasePath
 *                      resolves for the pair.
 * @param read_only     If @c true, all files are mapped read-only.
 */
MMappedCircuit(Oid db_oid, Oid db_tablespace, bool read_only = false);

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
 * @brief Write the @c info1 / @c info2 annotations of a gate, once.
 *
 * The two fields are written once **each**, with @c 0 meaning "nothing
 * recorded": a field goes from @c 0 to a value once, accepts that value
 * again, and refuses a different one; writing @c 0 over a value records
 * nothing and leaves it alone.  Per field rather than per pair because
 * the two are written by different parties -- a certified @c plus gate
 * is marked as certified when it is built and tagged with the route that
 * made it a query's root afterwards.
 *
 * @param token     UUID of the gate to annotate.
 * @param info1     Value for @c info1, or @c 0 to leave it alone.
 * @param info2     Value for @c info2, or @c 0 to leave it alone.
 * @param existing  On @c AlreadySet, the pair the gate holds.
 */
SetAnnotationResult setInfos(pg_uuid_t token, unsigned info1, unsigned info2,
                             std::pair<unsigned, unsigned> *existing = nullptr);

/**
 * @brief Attach a variable-length string annotation to a gate, once.
 *
 * A gate with no annotation has an empty one, and an empty string is not
 * an annotation, so "nothing recorded" and "recorded as nothing"
 * coincide and the question of an unset marker does not arise.  Offering
 * the bytes the gate already holds is a no-op rather than a fresh
 * append, which is what keeps the @c extra file from accumulating
 * abandoned copies of the same string.
 *
 * @param token     UUID of the gate.
 * @param s         String to store.
 * @param existing  On @c AlreadySet, the string the gate holds.
 */
SetAnnotationResult setExtra(pg_uuid_t token, const std::string &s,
                             std::string *existing = nullptr);

/**
 * @brief Write a gate's probability, once.
 *
 * A probability is a fact appended to the circuit, like the gate
 * itself: it can be written when the gate has none, re-written with
 * the identical value (so setup scripts and notebook cells stay
 * re-runnable), and otherwise refused.  Changing one means minting a
 * fresh input gate and rewriting the rows that carry the old token --
 * see @c provsql.replace_input.
 *
 * Passing @c NaN clears the probability unconditionally; that is how a
 * transaction that rolls back drops the probabilities it wrote, and it
 * is not reachable from SQL.
 *
 * If the token is not yet in the circuit, an input gate is created
 * lazily.
 *
 * @param token     UUID of the gate.
 * @param prob      Probability value in [0, 1], or @c NaN to clear.
 * @param existing  On @c AlreadySet, the probability the gate holds.
 * @return          Which of the four cases applied.
 */
SetProbResult setProb(pg_uuid_t token, double prob, double *existing = nullptr);

/**
 * @brief Report whether @p token names a gate that holds a probability.
 * @param token  UUID of the gate.
 * @param prob   On @c true return, the stored probability.
 * @return @c true when the gate exists, carries probabilities, and one
 *         has been written to it.
 */
bool hasProb(pg_uuid_t token, double *prob) const;

/**
 * @brief Flush all backing files to disk with @c msync().
 */
void sync();

/**
 * @brief Force every backing file to stable storage.
 *
 * @c sync() pushes the regions' bytes into the files; this pushes the
 * files out of the kernel's page cache, which is what a crash of the
 * machine can otherwise lose.
 */
void flush();

/**
 * @brief Whether any backing file was found still marked open-for-writing
 *        when it was opened -- the previous writer died mid-write.
 */
bool uncleanShutdown() const;

/**
 * @brief What @c provsql.check_store() reports about a store.
 *
 * Every count but @c unclean is zero for a store nothing has damaged.
 * A non-zero one means a write was interrupted at a point the ordering
 * rules do not cover, or a set of files was copied at different instants
 * (a file-level backup of a running server); @c provsql.circuit_cleanup()
 * rebuilds the store from what is still reachable.
 */
struct Check {
  bool unclean;                   ///< A file was left marked open-for-writing
  unsigned long nb_gates;         ///< Gate records
  unsigned long nb_mapping;       ///< Mapping entries
  unsigned long next_value;       ///< Next index the mapping would assign
  unsigned long dangling_indices; ///< Mapping entries indexing past the records
  unsigned long unreferenced;     ///< Records no mapping entry points at
  unsigned long bad_wires;        ///< Records whose children run past the wires
  unsigned long bad_extra;        ///< Records whose extra runs past the extra file
};

/** @brief Walk the store and report what does not add up. */
Check check() const;

/**
 * @brief Mark every gate reachable from @p roots.
 *
 * @param roots  Token UUIDs to start from; unknown ones are ignored.
 * @param live   Filled with one flag per gate record.
 * @return       The number of live gates.
 */
unsigned long mark(const std::vector<pg_uuid_t> &roots,
                   std::vector<bool> &live) const;

/** @brief The size of a store, in the three units that matter. */
struct Counts {
  unsigned long gates;       ///< Gate records
  unsigned long wires;       ///< Child wires
  unsigned long extra_bytes; ///< Bytes of variable-length annotation
};

/** @brief This store's current size. */
inline Counts counts() const {
  return { gates.nbElements(), wires.nbElements(), extra.nbElements() };
}

/**
 * @brief Copy the gates flagged in @p live into a fresh set of files
 *        beside the current ones (suffix @c ".new").
 *
 * Child indices, extra offsets and the token table are all rebuilt, so
 * the result is a compact, freshly hashed store.  A version-1 @c gates
 * file also gets its probabilities normalised on the way through: the
 * @c 1.0 that used to mean both "written as certain" and "never written"
 * becomes @c NaN on gates the file cannot prove were written, and the new
 * file is stamped version 2, after which the ambiguity is gone.
 *
 * @return The gate, wire and extra-byte counts of the new files.
 */
Counts sweepInto(const std::vector<bool> &live,
                 const std::string &mp, const std::string &gp,
                 const std::string &wp, const std::string &ep) const;

/** @brief Whether this store's @c gates file predates the @c NaN
 *  unset-probability convention (see @c GATES_VERSION). */
inline bool legacyProbabilities() const {
  return gates.version() < GATES_VERSION;
}



/**
 * @brief Return the type of the gate identified by @p token.
 * @param token  UUID of the gate.
 * @return       The gate's type, or @c gate_input if not found (lazy default).
 */
gate_type getGateType(pg_uuid_t token) const;

/**
 * @brief Return the child UUIDs of the gate identified by @p token.
 * @param token  UUID of the gate.
 * @return       Ordered vector of child UUIDs.
 */
std::vector<pg_uuid_t> getChildren(pg_uuid_t token) const;

/**
 * @brief Return the probability an evaluation would use for @p token.
 *
 * A gate nobody has given a probability evaluates as certain, and a
 * repaired row nobody has given one evaluates at the uniform weight of
 * its @c repair_key block; either way this answers a number, not "unset".
 * Use @c hasProb to tell the two apart.
 *
 * @param token  UUID of the gate.
 * @return       The probability; @c NaN when @p token names no gate, or
 *               one that carries no probability at all.
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
GenericCircuit createGenericCircuit(pg_uuid_t token) const;

/**
 * @brief Build an in-memory @c GenericCircuit reachable from any of
 *        @p roots.
 *
 * Multi-root variant of @c createGenericCircuit.  Seeds the BFS with
 * every UUID in @p roots so a shared subgraph reachable from more
 * than one root is represented by a single @c gate_t (the
 * @c GenericCircuit::setGate / @c getGate pair is idempotent on the
 * UUID key).  Used by @c getJointCircuit to load an RV's sub-DAG
 * together with a conditioning gate that sits above it in the
 * persisted DAG.
 *
 * @param roots  UUIDs whose reachable closure to load.  Order is
 *               irrelevant; identical UUIDs collapse via the
 *               @c std::set deduplication of the work list.
 * @return       An in-memory @c GenericCircuit containing every gate
 *               reachable from any root.
 */
GenericCircuit createGenericCircuit(
    const std::vector<pg_uuid_t> &roots) const;
};

#ifdef PROVSQL_INPROCESS_STORE
/**
 * @brief Build a @c GenericCircuit rooted at @p token directly from the
 *        current backend's store.
 *
 * Single-process replacement for the @c 'g' IPC round-trip: there is no
 * worker/backend boundary to ship a Boost-serialised copy across, so the
 * backend constructs the circuit in place.
 */
GenericCircuit provsql_inproc_generic_circuit(pg_uuid_t token);
/** @brief Multi-root variant (single-process replacement for @c 'j'). */
GenericCircuit provsql_inproc_joint_circuit(
  const std::vector<pg_uuid_t> &roots);
#endif


#endif /* MMAPPED_CIRCUIT_H */
