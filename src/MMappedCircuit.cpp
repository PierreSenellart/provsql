/**
 * @file MMappedCircuit.cpp
 * @brief Persistent mmap-backed circuit: implementation and background-worker entry points.
 *
 * Implements the @c MMappedCircuit methods declared in @c MMappedCircuit.h,
 * the @c createGenericCircuit() free function, and the background-worker
 * entry points declared in @c provsql_mmap.h:
 *
 * - @c initialize_provsql_mmap(): called by the background worker at
 *   startup; the per-database @c MMappedCircuit instances themselves are
 *   opened lazily, on the first message for their database.
 * - @c destroy_provsql_mmap(): called on shutdown; syncs and deletes the
 *   singleton.
 * - @c provsql_mmap_main_loop(): the worker's main loop; receives gate-
 *   creation messages from backends over the IPC pipe and writes them
 *   to the mmap store.
 *
 * The @c createGenericCircuit() function performs a BFS from a root UUID,
 * reading gates from the mmap store and building an in-memory @c GenericCircuit.
 */
#include <cerrno>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>

#include "MMappedCircuit.h"
#include "process_memory.h"
#include "GenericCircuit.h"
#include "Circuit.hpp"
#include "provsql_utils_cpp.h"

#include <poll.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#ifndef PROVSQL_INPROCESS_STORE
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/stream.hpp>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

extern "C" {
#include "miscadmin.h"
#include "common/relpath.h"
#include "utils/palloc.h"
#include "provsql_mmap.h"

/* Whether the message being applied comes from the replay spool, which only
   the build with a separate worker has. */
#ifdef PROVSQL_INPROCESS_STORE
#define PROVSQL_FROM_SPOOL false
#else
#define PROVSQL_FROM_SPOOL provsql_worker_from_spool
#endif
#include "provsql_shmem.h"
}

/** @brief Per-database mmap-backed provenance circuits, keyed by database OID. */
static std::map<Oid, MMappedCircuit*> circuits;

/** @brief Whether any circuit has been written to since the last flush.
 *
 *  The store's bytes reach the kernel as soon as a message is applied, but
 *  they reach the disk only when someone forces them; a machine that loses
 *  power in between loses whatever the kernel had not written back.  The
 *  worker forces them a short while after the last write (see the main
 *  loop below), which bounds that loss the way
 *  @c synchronous_commit @c = @c off bounds the heap's. */
static bool store_dirty = false;

std::string MMappedCircuit::storePath(Oid db_oid, Oid db_tablespace,
                                      const char *filename)
{
  /* GetDatabasePath yields the directory relative to the data directory:
     "base/<oid>" for the default tablespace, and
     "pg_tblspc/<ts>/PG_<ver>_<cat>/<oid>" for a database created in (or
     moved to) another one.  Resolving it here, rather than hardcoding
     base/, is what makes the store follow CREATE DATABASE ... TABLESPACE
     and ALTER DATABASE ... SET TABLESPACE. */
  char *rel = GetDatabasePath(db_oid, db_tablespace);
  std::string path = std::string(DataDir) + "/" + rel + "/" + filename;
  pfree(rel);
  return path;
}

MMappedCircuit::MMappedCircuit(Oid db_oid, Oid db_tablespace, bool read_only) :
  MMappedCircuit(
    storePath(db_oid, db_tablespace, MAPPING_FILENAME),
    storePath(db_oid, db_tablespace, GATES_FILENAME),
    storePath(db_oid, db_tablespace, WIRES_FILENAME),
    storePath(db_oid, db_tablespace, EXTRA_FILENAME),
    read_only) {}

bool MMappedCircuit::storeExists(Oid db_oid, Oid db_tablespace)
{
  std::string gates = storePath(db_oid, db_tablespace, GATES_FILENAME);
  return !(access(gates.c_str(), F_OK) != 0 && errno == ENOENT);
}

void initialize_provsql_mmap()
{
  /* circuits are opened lazily on first IPC message */
}

void destroy_provsql_mmap()
{
  for(auto &kv: circuits)
    delete kv.second;
  circuits.clear();
}

/** @brief Add a gate record and only then make it reachable under @p token.
 *
 *  The record has to be complete before the mapping entry that points at
 *  it exists: killed between the two, the store keeps an unreferenced
 *  record, which costs nothing, whereas the other order leaves a mapping
 *  entry indexing past the end of the record vector and every gate
 *  created afterwards resolves one slot off, for the rest of the store's
 *  life. */
void MMappedCircuit::appendGate(pg_uuid_t token, gate_type type,
                                const std::vector<pg_uuid_t> &children)
{
  const unsigned long idx = gates.nbElements();
  const unsigned long wires_idx = wires.nbElements();
  for(const auto &c: children)
    wires.add(c);
  gates.add({type, static_cast<unsigned>(children.size()), wires_idx});
  mapping.publish(token, idx);
}

void MMappedCircuit::createGate(
  pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    // The gate may have been lazy-added as a default gate_input below
    // (when an earlier-arriving parent createGate referenced it as a
    // child whose own createGate had not yet been received). Under
    // concurrent backends, parent/child IPCs from different sessions
    // can be interleaved such that the parent's lazy-add wins and the
    // real create for the child is then silently dropped. Detect that
    // case and upgrade the placeholder in place; otherwise leave the
    // existing gate alone (real duplicate creation, idempotent).
    bool placeholder = gates[idx].type == gate_input
                       && gates[idx].nb_children == 0;
    bool real_create = type != gate_input || !children.empty();
    if(placeholder && real_create) {
      const unsigned long wires_idx = wires.nbElements();
      for(const auto &c: children)
        wires.add(c);
      // Wires first, then the record's own fields: an upgrade seen
      // half-done would otherwise claim children that are not there yet.
      gates[idx].children_idx = wires_idx;
      gates[idx].nb_children = static_cast<unsigned>(children.size());
      gates[idx].type = type;
      for(const auto &c: children)
        if(mapping[c] == MMappedUUIDHashTable::NOTHING)
          appendGate(c, gate_input, {});
    }
    return;
  }

  appendGate(token, type, children);

  for(const auto &c: children)
    if(mapping[c] == MMappedUUIDHashTable::NOTHING)
      appendGate(c, gate_input, {});
}

gate_type MMappedCircuit::getGateType(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return gate_input;
  else
    return gates[idx].type;
}

std::vector<pg_uuid_t> MMappedCircuit::getChildren(pg_uuid_t token) const
{
  std::vector<pg_uuid_t> result;
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    const GateInformation &gi = gates[idx];
    for(unsigned long k=gi.children_idx; k<gi.children_idx+gi.nb_children; ++k)
      result.push_back(wires[k]);
  }
  return result;
}

/** @brief Whether @p type is one of the gate kinds that carry a probability. */
static bool carriesProb(gate_type type)
{
  return type == gate_input || type == gate_update || type == gate_mulinput;
}

MMappedCircuit::SetProbResult MMappedCircuit::setProb(
  pg_uuid_t token, double prob, double *existing)
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING) {
    idx = gates.nbElements();
    appendGate(token, gate_input, {});
  }

  if(!carriesProb(gates[idx].type))
    return SetProbResult::NotProbGate;

  /* Clearing (the rollback path) always succeeds: it restores the gate
     to the state it was in before the aborted transaction wrote to it. */
  if(std::isnan(prob)) {
    gates[idx].prob = NAN;
    return SetProbResult::Written;
  }

  if(!hasProbAt(idx)) {
    gates[idx].prob = prob;
    return SetProbResult::Written;
  }
  if(gates[idx].prob == prob)
    return SetProbResult::Unchanged;
  if(existing)
    *existing = gates[idx].prob;
  return SetProbResult::AlreadySet;
}

/** @brief Whether the record at @p idx holds a probability someone wrote.
 *
 *  A version-1 @c gates file cannot distinguish "written as 1.0" from
 *  "never written" -- both are stored as @c 1.0 -- so on such a file a
 *  @c 1.0 counts as unset and stays writable.  See @c GATES_VERSION. */
bool MMappedCircuit::hasProbAt(unsigned long idx) const
{
  double p = gates[idx].prob;
  if(std::isnan(p))
    return false;
  if(gates.version() < 2 && p == 1.)
    return false;
  return true;
}

double MMappedCircuit::getProb(pg_uuid_t token) const
{
  return probAt(mapping[token]);
}

double MMappedCircuit::probAt(unsigned long idx) const
{
  if(idx == MMappedUUIDHashTable::NOTHING || !carriesProb(gates[idx].type))
    return NAN;
  if(!std::isnan(gates[idx].prob))
    return gates[idx].prob;
  /* A repaired row nobody gave a probability evaluates at the uniform
     weight of its block, whose size repair_key recorded in info2. */
  if(gates[idx].type == gate_mulinput && gates[idx].info2 > 0)
    return 1. / gates[idx].info2;
  /* Otherwise an unwritten probability evaluates as 1: a tuple nobody
     gave a probability is certainly there. */
  return 1.;
}

bool MMappedCircuit::hasProb(pg_uuid_t token, double *prob) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING || !carriesProb(gates[idx].type))
    return false;
  if(!hasProbAt(idx))
    return false;
  if(prob)
    *prob = gates[idx].prob;
  return true;
}

/* The two info fields are written once each, not once as a pair,
   because they are written by different parties at different moments:
   CertifiedDDMaterialize marks a gate certified (info1) as it builds it,
   and tags it afterwards with the route that made it a query's root
   (info2), which it only knows once the whole d-D is built.  Zero is
   "nothing recorded" for each field, so a field can go 0 -> v once and
   a write of 0 over a v records nothing rather than clearing it. */
MMappedCircuit::SetAnnotationResult MMappedCircuit::setInfos(
  pg_uuid_t token, unsigned info1, unsigned info2,
  std::pair<unsigned, unsigned> *existing)
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return SetAnnotationResult::NoSuchGate;

  GateInformation &gi = gates[idx];

  if((info1 != 0 && gi.info1 != 0 && gi.info1 != info1)
     || (info2 != 0 && gi.info2 != 0 && gi.info2 != info2)) {
    if(existing)
      *existing = std::make_pair(gi.info1, gi.info2);
    return SetAnnotationResult::AlreadySet;
  }

  const unsigned w1 = info1 != 0 ? info1 : gi.info1;
  const unsigned w2 = info2 != 0 ? info2 : gi.info2;
  if(w1 == gi.info1 && w2 == gi.info2)
    return SetAnnotationResult::Unchanged;

  gi.info1 = w1;
  gi.info2 = w2;
  return SetAnnotationResult::Written;
}

MMappedCircuit::SetAnnotationResult MMappedCircuit::setExtra(
  pg_uuid_t token, const std::string &s, std::string *existing)
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return SetAnnotationResult::NoSuchGate;

  /* Already carries exactly these bytes: nothing to do.  Every caller
     writes the extra string right after creating a content-addressed
     gate, so recomputing the same expression offers the same bytes back;
     appending them a second time would abandon the first copy in the
     extra file for good, which is the store's main source of dead
     space. */
  if(gates[idx].extra_len == s.size()) {
    bool same = true;
    for(unsigned long k=0; k<s.size(); ++k)
      if(extra[gates[idx].extra_idx + k] != s[k]) {
        same = false;
        break;
      }
    if(same)
      return SetAnnotationResult::Unchanged;
  }

  if(gates[idx].extra_len > 0) {
    if(existing)
      *existing = getExtra(token);
    return SetAnnotationResult::AlreadySet;
  }

  gates[idx].extra_idx=extra.nbElements();
  for(auto c: s)
    extra.add(c);
  gates[idx].extra_len=s.size();
  return SetAnnotationResult::Written;
}

std::pair<unsigned, unsigned> MMappedCircuit::getInfos(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING) {
    return std::make_pair(0, 0);
  } else {
    const GateInformation &gi = gates[idx];
    return std::make_pair(gi.info1, gi.info2);
  }
}

std::string MMappedCircuit::getExtra(pg_uuid_t token) const
{
  return extraAt(mapping[token]);
}

std::string MMappedCircuit::extraAt(unsigned long idx) const
{
  std::string result;

  if(idx != MMappedUUIDHashTable::NOTHING) {
    for(unsigned long start=gates[idx].extra_idx, k=start, end=start+gates[idx].extra_len; k<end; ++k)
      result+=extra[k];
  }

  return result;
}

/** @brief Suffix of the files a clean-up builds beside the live ones. */
static constexpr const char *CLEANUP_SUFFIX = ".new";
/** @brief Marker file present only while a clean-up's rename sequence is
 *  in flight; see @c finishInterruptedCleanup. */
static constexpr const char *CLEANUP_MARKER = "provsql_cleanup.commit";

/**
 * @brief Bring a database's store to a definite state before opening it.
 *
 * A clean-up writes four new files, then renames them over the live ones
 * one at a time.  A crash inside that sequence would leave a mixture, so
 * the sequence is bracketed by a marker file: present, it says the new
 * files are complete and the renames must be finished; absent, it says
 * any @c ".new" files are the debris of a clean-up that never got that
 * far and are to be discarded.
 */
static void finishInterruptedCleanup(Oid db_oid, Oid db_tablespace)
{
  const char *names[4] = { "provsql_mapping.mmap", "provsql_gates.mmap",
                           "provsql_wires.mmap", "provsql_extra.mmap" };
  std::string marker = MMappedCircuit::storePath(db_oid, db_tablespace,
                                                 CLEANUP_MARKER);
  const bool committed = (access(marker.c_str(), F_OK) == 0);

  for(const char *name: names) {
    std::string target = MMappedCircuit::storePath(db_oid, db_tablespace, name);
    std::string fresh = target + CLEANUP_SUFFIX;
    if(access(fresh.c_str(), F_OK) != 0)
      continue;
    if(committed)
      rename(fresh.c_str(), target.c_str());
    else
      unlink(fresh.c_str());
  }

  if(committed)
    unlink(marker.c_str());
}

/** @brief The tablespace of each open circuit, to find its files again. */
static std::map<Oid, Oid> circuit_tablespaces;

/**
 * @brief Close the circuits of the databases dropped since they were opened.
 *
 * Dropping a database removes its directory, the circuit files with it, but
 * not the worker's descriptors and mappings of them: kept, they would hold
 * the disk space of every dropped database, and four descriptors each, until
 * opening one more failed for lack of descriptors -- which a test run that
 * creates and drops a database per test reached after a few hundred.  A
 * circuit whose gates file is gone belongs to a dropped database (one being
 * cleaned up keeps it until the swap, and is not open meanwhile).
 */
static void closeDroppedCircuits()
{
  for(auto it = circuits.begin(); it != circuits.end(); ) {
    auto ts = circuit_tablespaces.find(it->first);
    if(!MMappedCircuit::storeExists(
         it->first,
         ts == circuit_tablespaces.end() ? InvalidOid : ts->second)) {
      delete it->second;
      circuit_tablespaces.erase(it->first);
      it = circuits.erase(it);
    } else
      ++it;
  }
}

/** @brief Return (creating lazily if needed) the circuit for @p db_oid. */
static MMappedCircuit *getCircuit(Oid db_oid, Oid db_tablespace)
{
  auto it = circuits.find(db_oid);
  if(it == circuits.end()) {
    /* Opening a database the worker has not seen yet is when the open
       circuits grow: release those of the dropped databases first. */
    closeDroppedCircuits();
    finishInterruptedCleanup(db_oid, db_tablespace);
    circuits[db_oid] = new MMappedCircuit(db_oid, db_tablespace);
    circuit_tablespaces[db_oid] = db_tablespace;
    return circuits[db_oid];
  }
  return it->second;
}

/**
 * @brief Rebuild a database's store, keeping only what @p roots reach.
 *
 * The store only ever grows: a gate is never removed, because removing
 * one is unsafe while anything might still reference it, and because a
 * content-addressed gate can be re-adopted by a query at any moment.
 * This is the one operation allowed to remove gates, and it is safe only
 * because the caller holds the database exclusively -- no other session
 * is connected, so nothing can adopt an orphan while the sweep runs.
 *
 * On @p dry_run the mark phase runs and the counts are reported, but
 * nothing is written.
 */
static void cleanupStore(Oid db_oid, Oid db_tablespace,
                         const std::vector<pg_uuid_t> &roots, bool dry_run,
                         provsql_cleanup_result *out)
{
  MMappedCircuit *circuit = getCircuit(db_oid, db_tablespace);

  auto before = circuit->counts();
  out->gates_before = before.gates;
  out->wires_before = before.wires;
  out->extra_before = before.extra_bytes;

  std::vector<bool> live;
  unsigned long nb_live = circuit->mark(roots, live);

  if(dry_run) {
    /* Report what a real run would keep, without the wire and extra
       counts of the rewrite -- those need the sweep to be exact.  The
       live gate count is what the operator is deciding on. */
    out->gates_after = nb_live;
    out->wires_after = 0;
    out->extra_after = 0;
    return;
  }

  const char *names[4] = { "provsql_mapping.mmap", "provsql_gates.mmap",
                           "provsql_wires.mmap", "provsql_extra.mmap" };
  std::string target[4], fresh[4];
  for(int i=0; i<4; ++i) {
    target[i] = MMappedCircuit::storePath(db_oid, db_tablespace, names[i]);
    fresh[i] = target[i] + CLEANUP_SUFFIX;
    unlink(fresh[i].c_str());
  }

  auto after = circuit->sweepInto(live, fresh[0], fresh[1], fresh[2], fresh[3]);
  out->gates_after = after.gates;
  out->wires_after = after.wires;
  out->extra_after = after.extra_bytes;

  /* Close the live store before the swap, so its dirty bits are cleared
     and the next message reopens the rebuilt files. */
  delete circuit;
  circuits.erase(db_oid);
  circuit_tablespaces.erase(db_oid);

  std::string marker = MMappedCircuit::storePath(db_oid, db_tablespace,
                                                 CLEANUP_MARKER);
  int mfd = open(marker.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600); // flawfinder: ignore
  if(mfd == -1) {
    const char *reason = strerror(errno);
    provsql_error("circuit_cleanup: cannot create %s: %s",
                  marker.c_str(), reason);
  }
  fsync(mfd);
  close(mfd);

  for(int i=0; i<4; ++i)
    if(rename(fresh[i].c_str(), target[i].c_str())) {
      const char *reason = strerror(errno);
      provsql_error("circuit_cleanup: cannot replace %s: %s",
                    target[i].c_str(), reason);
    }

  unlink(marker.c_str());
}

#ifdef PROVSQL_INPROCESS_STORE
/* Single-process build: the backend builds the in-memory circuit directly
   from this process's store, instead of round-tripping a Boost-serialised
   copy through the FIFO (which only existed to cross the worker/backend
   process boundary).  This is also what lets the WASM build avoid the
   compiled libboost_serialization dependency. */
GenericCircuit provsql_inproc_generic_circuit(pg_uuid_t token,
                                              std::size_t budget)
{
  return getCircuit(MyDatabaseId, MyDatabaseTableSpace)->createGenericCircuit(token, budget);
}

GenericCircuit provsql_inproc_joint_circuit(
  const std::vector<pg_uuid_t> &roots, std::size_t budget)
{
  return getCircuit(MyDatabaseId, MyDatabaseTableSpace)->createGenericCircuit(roots, budget);
}

FormulaRegions provsql_inproc_formula_regions(
  const std::vector<FormulaRequest> &requests, unsigned long cap,
  unsigned long wide_cap)
{
  return getCircuit(MyDatabaseId, MyDatabaseTableSpace)->formulaRegions(
    requests, cap, wide_cap);
}
#endif

extern "C" void provsql_store_flush(void)
{
  for(auto &kv: circuits)
    kv.second->flush();
  store_dirty = false;
}

#ifndef PROVSQL_INPROCESS_STORE
/**
 * @brief Answer a @c 'g' / @c 'j' request: the circuit reachable from
 *        @p roots, serialised, preceded by its size.
 *
 * The circuit is serialised straight into the buffer that is sent, without
 * the copy a @c std::stringstream makes of its content, and is freed before
 * it is sent.  With a @p budget (bytes, 0 for none: what the requesting
 * statement has left of @c provsql.max_memory), building and serialising it
 * may not make this process grow by more; past it, the answer is
 * @c PROVSQL_CIRCUIT_OVER_BUDGET and no circuit.  The memory is given back
 * afterwards, so that one large request does not stay resident in the
 * worker.
 */
static void send_circuit(const MMappedCircuit *circuit,
                         const std::vector<pg_uuid_t> &roots,
                         unsigned long budget, char message_char)
{
  const std::size_t base_rss = budget ? provsql_process_rss(getpid()) : 0;
  std::vector<char> buf;
  unsigned long size;

  try {
    {
      boost::iostreams::stream<
        boost::iostreams::back_insert_device<std::vector<char>>> out(buf);
      boost::archive::binary_oarchive oa(out);
      oa << circuit->createGenericCircuit(roots, budget);
      out.flush();
    }
    size = buf.size();
    if(budget) {
      std::size_t rss = provsql_process_rss(getpid());
      if(rss > base_rss && rss - base_rss > budget)
        size = PROVSQL_CIRCUIT_OVER_BUDGET;
    }
  } catch(const CircuitBudgetExceeded &) {
    size = PROVSQL_CIRCUIT_OVER_BUDGET;
  }
  if(size == PROVSQL_CIRCUIT_OVER_BUDGET)
    std::vector<char>().swap(buf);

  if(!WRITEB(&size, unsigned long) ||
     (size != PROVSQL_CIRCUIT_OVER_BUDGET && !WRITEB_BYTES(buf.data(), size)))
    provsql_error("Cannot write to pipe (message type %c)", message_char);

  std::vector<char>().swap(buf);
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}
#endif

/** @brief The body of @c provsql_mmap_dispatch, which may throw. */
static void dispatch(char c, Oid db_oid, Oid db_tablespace)
{
    MMappedCircuit *circuit = getCircuit(db_oid, db_tablespace);

    if(c=='C' || c=='G' || c=='P' || c=='I' || c=='E')
      store_dirty = true;

    switch(c) {
    case 'C':
    {
      pg_uuid_t token;
      gate_type type;
      unsigned nb_children;

      if(!READM(token, pg_uuid_t) || !READM(type, gate_type) || !READM(nb_children, unsigned))
        provsql_error("Cannot read from pipe (message type C)"); ;

      std::vector<pg_uuid_t> children(nb_children);
      for(unsigned i=0; i<nb_children; ++i)
        if(!READM(children[i], pg_uuid_t))
          provsql_error("Cannot read from pipe (message type C)");

      circuit->createGate(token, type, children);
      break;
    }

    case 'G':
    {
      /* A gate with what it records, in one message that is not answered:
         the sender's address determines the infos and the text, so the
         write-once rule cannot refuse them.  If it does, someone wrote
         something else at that address by hand (or hashes collided): the
         first value stays, and the worker's log says so. */
      pg_uuid_t token;
      gate_type type;
      unsigned nb_children, info1, info2, len;
      char has_infos;

      if(!READM(token, pg_uuid_t) || !READM(type, gate_type) || !READM(nb_children, unsigned))
        provsql_error("Cannot read from pipe (message type G)");

      std::vector<pg_uuid_t> children(nb_children);
      for(unsigned i=0; i<nb_children; ++i)
        if(!READM(children[i], pg_uuid_t))
          provsql_error("Cannot read from pipe (message type G)");
      if(!READM(has_infos, char) || !READM(info1, unsigned) || !READM(info2, unsigned)
         || !READM(len, unsigned))
        provsql_error("Cannot read from pipe (message type G)");
      std::vector<char> data(len);
      if(len > 0 && !READM_BYTES(data.data(), len))
        provsql_error("Cannot read from pipe (message type G)");

      circuit->createGate(token, type, children);
      if(has_infos) {
        std::pair<unsigned, unsigned> existing{0, 0};
        if(circuit->setInfos(token, info1, info2, &existing)
           == MMappedCircuit::SetAnnotationResult::AlreadySet)
          provsql_warning("gate %s already records the annotation (%u, %u), not (%u, %u)",
                          uuid2string(token).c_str(), existing.first, existing.second,
                          info1, info2);
      }
      if(len > 0) {
        std::string existing;
        if(circuit->setExtra(token, std::string(data.data(), len), &existing)
           == MMappedCircuit::SetAnnotationResult::AlreadySet)
          provsql_warning("gate %s already records the annotation \"%s\", not \"%s\"",
                          uuid2string(token).c_str(), existing.c_str(),
                          std::string(data.data(), len).c_str());
      }
      break;
    }

    case 'P':
    {
      pg_uuid_t token;
      double prob, existing = 0.;

      if(!READM(token, pg_uuid_t) || !READM(prob, double))
        provsql_error("Cannot read from pipe (message type P)");

      auto result = circuit->setProb(token, prob, &existing);
      char return_value = static_cast<char>(result);

      /* A message of the replay spool has nobody waiting for its answer */
      if(!PROVSQL_FROM_SPOOL &&
         (!WRITEB(&return_value, char) || !WRITEB(&existing, double)))
        provsql_error("Cannot write response to pipe (message type P)");
      break;
    }

    case 'q':
    {
      /* Is a probability written on this gate, and which one?  Distinct
         from 'p', which reports the value an evaluation would use and so
         cannot tell an unwritten probability from one written as 1. */
      pg_uuid_t token;
      double prob = 0.;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type q)");

      char has = circuit->hasProb(token, &prob) ? 1 : 0;

      if(!WRITEB(&has, char) || !WRITEB(&prob, double))
        provsql_error("Cannot write response to pipe (message type q)");
      break;
    }

    case 'I':
    case 'E':
    {
      /* Infos or text written after the gate.  Nothing in this version
         sends them (a gate is created with what it records, message G);
         the install scripts of earlier versions, run by an upgrade, and
         WAL records of earlier versions still do.  Applied write-once
         like G, and not answered. */
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type %c)", c);
      if(c == 'I') {
        unsigned info1, info2;
        std::pair<unsigned, unsigned> existing{0, 0};
        if(!READM(info1, unsigned) || !READM(info2, unsigned))
          provsql_error("Cannot read from pipe (message type I)");
        if(circuit->setInfos(token, info1, info2, &existing)
           == MMappedCircuit::SetAnnotationResult::AlreadySet)
          provsql_warning("gate %s already records the annotation (%u, %u), not (%u, %u)",
                          uuid2string(token).c_str(), existing.first, existing.second,
                          info1, info2);
      } else {
        unsigned len;
        if(!READM(len, unsigned))
          provsql_error("Cannot read from pipe (message type E)");
        if(len > 0) {
          std::vector<char> data(len);
          std::string existing;
          if(!READM_BYTES(data.data(), len))
            provsql_error("Cannot read from pipe (message type E)");
          if(circuit->setExtra(token, std::string(data.data(), len), &existing)
             == MMappedCircuit::SetAnnotationResult::AlreadySet)
            provsql_warning("gate %s already records the annotation \"%s\", not \"%s\"",
                            uuid2string(token).c_str(), existing.c_str(),
                            std::string(data.data(), len).c_str());
        }
      }
      break;
    }

    case 't':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type t)");

      gate_type type = circuit->getGateType(token);

      if(!WRITEB(&type, gate_type))
        provsql_error("Cannot write response to pipe (message type t)");
      break;
    }

    case 'n':
    {
      unsigned long nb = circuit->getNbGates();

      if(!WRITEB(&nb, unsigned long))
        provsql_error("Cannot write response to pipe (message type n)");
      break;
    }

    case 'c':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type c)");

      auto children = circuit->getChildren(token);
      unsigned nb_children = children.size();
      if(!WRITEB(&nb_children, unsigned))
        provsql_error("Cannot write response to pipe (message type c)");

      if(!WRITEB_BYTES(children.data(), nb_children*sizeof(pg_uuid_t)))
        provsql_error("Cannot write response to pipe (message type c)");
      break;
    }

    case 'p':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type p)");

      double prob = circuit->getProb(token);

      if(!WRITEB(&prob, double))
        provsql_error("Cannot write response to pipe (message type p)");
      break;
    }

    case 'i':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type i)");

      auto infos = circuit->getInfos(token);

      if(!WRITEB(&infos.first, unsigned) || !WRITEB(&infos.second, unsigned))
        provsql_error("Cannot write response to pipe (message type i)");
      break;
    }

    case 'e':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type e)");

      auto str = circuit->getExtra(token);
      unsigned len = str.size();

      if(!WRITEB(&len, unsigned) || !WRITEB_BYTES(str.data(), len))
        provsql_error("Cannot write response to pipe (message type e)");
      break;
    }

    case 'g':
    {
      pg_uuid_t token;
      unsigned long budget;

      if(!READM(token, pg_uuid_t) || !READM(budget, unsigned long))
        provsql_error("Cannot read from pipe (message type g)");

#ifdef PROVSQL_INPROCESS_STORE
      /* Unreachable: the backend calls provsql_inproc_generic_circuit
         directly instead of issuing the 'g' message. */
      provsql_error("message type g is not used by the in-process store");
#else
      send_circuit(circuit, std::vector<pg_uuid_t>{token}, budget, 'g');
#endif
      break;
    }







    case 'X':
    {
      /* Clean-up: rebuild the store keeping only what the roots reach.
         The caller holds the database exclusively (see
         provsql.circuit_cleanup), so nothing can adopt an orphan while
         this runs. */
      char dry_run;
      unsigned long nb_roots;

      if(!READM(dry_run, char) || !READM(nb_roots, unsigned long))
        provsql_error("Cannot read from pipe (message type X)");

      std::vector<pg_uuid_t> roots(nb_roots);
      for(unsigned long i=0; i<nb_roots; ++i)
        if(!READM(roots[i], pg_uuid_t))
          provsql_error("Cannot read from pipe (message type X)");

      provsql_cleanup_result res{};
      cleanupStore(db_oid, db_tablespace, roots, dry_run != 0, &res);

      if(!WRITEB(&res.gates_before, uint64) || !WRITEB(&res.gates_after, uint64)
         || !WRITEB(&res.wires_before, uint64) || !WRITEB(&res.wires_after, uint64)
         || !WRITEB(&res.extra_before, uint64) || !WRITEB(&res.extra_after, uint64))
        provsql_error("Cannot write response to pipe (message type X)");
      break;
    }

    case 'S':
    {
      /* Sync barrier: force everything written so far to stable storage
         and say so.  Because the pipe is FIFO and the worker single
         threaded, the reply also proves every earlier message from this
         backend has been *applied*, not merely queued. */
      provsql_store_flush();

      char ack = 1;
      if(!WRITEB(&ack, char))
        provsql_error("Cannot write response to pipe (message type S)");
      break;
    }

    case 'k':
    {
      /* Consistency report; see MMappedCircuit::Check. */
      MMappedCircuit::Check chk = circuit->check();
      char unclean = chk.unclean ? 1 : 0;
      if(!WRITEB(&unclean, char)
         || !WRITEB(&chk.nb_gates, unsigned long)
         || !WRITEB(&chk.nb_mapping, unsigned long)
         || !WRITEB(&chk.next_value, unsigned long)
         || !WRITEB(&chk.dangling_indices, unsigned long)
         || !WRITEB(&chk.unreferenced, unsigned long)
         || !WRITEB(&chk.bad_wires, unsigned long)
         || !WRITEB(&chk.bad_extra, unsigned long))
        provsql_error("Cannot write response to pipe (message type k)");
      break;
    }

    case 'j':
    {
      /* Joint-circuit load: BFS from a vector of roots so a shared
       * subgraph reachable from multiple roots collapses to a single
       * gate_t.  Used by getJointCircuit() to load an RV's sub-DAG
       * together with a conditioning gate that sits above it in the
       * persisted DAG. */
      unsigned nb_roots;
      if(!READM(nb_roots, unsigned))
        provsql_error("Cannot read from pipe (message type j)");

      std::vector<pg_uuid_t> roots(nb_roots);
      for(unsigned i=0; i<nb_roots; ++i)
        if(!READM(roots[i], pg_uuid_t))
          provsql_error("Cannot read from pipe (message type j)");
      unsigned long budget;
      if(!READM(budget, unsigned long))
        provsql_error("Cannot read from pipe (message type j)");

#ifdef PROVSQL_INPROCESS_STORE
      /* Unreachable: the backend calls provsql_inproc_joint_circuit
         directly instead of issuing the 'j' message. */
      provsql_error("message type j is not used by the in-process store");
#else
      send_circuit(circuit, roots, budget, 'j');
#endif
      break;
    }

    case 'F':
    {
      /* The regions of a few rows of sr_formula_rows */
      unsigned long cap, wide_cap;
      unsigned nb;
      if(!READM(cap, unsigned long) || !READM(wide_cap, unsigned long)
         || !READM(nb, unsigned))
        provsql_error("Cannot read from pipe (message type F)");
      std::vector<FormulaRequest> requests(nb);
      for(unsigned i = 0; i < nb; ++i)
        if(!READM(requests[i].gate, pg_uuid_t)
           || !READM(requests[i].from, unsigned long)
           || !READM(requests[i].count, unsigned long))
          provsql_error("Cannot read from pipe (message type F)");

#ifdef PROVSQL_INPROCESS_STORE
      provsql_error("message type F is not used by the in-process store");
#else
      std::vector<char> buf;
      {
        boost::iostreams::stream<
          boost::iostreams::back_insert_device<std::vector<char>>> out(buf);
        boost::archive::binary_oarchive oa(out);
        const FormulaRegions regions =
          circuit->formulaRegions(requests, cap, wide_cap);
        oa << regions;
        out.flush();
      }
      unsigned long size = buf.size();
      if(!WRITEB(&size, unsigned long) || !WRITEB_BYTES(buf.data(), size))
        provsql_error("Cannot write to pipe (message type F)");
#endif
      break;
    }

    default:
      provsql_error("Wrong message type: %c", c);
    }
}

/**
 * @brief Serve one message of the store's protocol.
 *
 * A C++ exception escaping here -- a file that cannot be opened or mapped,
 * an allocation that fails -- would reach the C frames of the worker and
 * terminate it with an abort, which the postmaster answers by resetting
 * every backend of the cluster.  It is reported as the error it is instead,
 * as the worker's other failures are: the worker exits and is restarted,
 * and the other databases keep their sessions.
 */
extern "C" void provsql_mmap_dispatch(char c, Oid db_oid, Oid db_tablespace)
{
  std::string what;
  try {
    dispatch(c, db_oid, db_tablespace);
    return;
  } catch(const std::exception &e) {
    what = e.what();
  } catch(...) {
    what = "unknown exception";
  }
  /* Outside the handler: the error does not return. */
  provsql_error("ProvSQL circuit store: %s", what.c_str());
}

#ifndef PROVSQL_INPROCESS_STORE
void provsql_mmap_main_loop()
{
  char c;

  for(;;) {
    struct pollfd pfd;
    pfd.fd = provsql_shared_state->pipebmr;
    pfd.events = POLLIN;
    pfd.revents = 0;

    /* Wait indefinitely while there is nothing to force out; once a write
       has landed, wake up after the flush interval and force it, so an
       idle store is never more than that far behind the disk.  Bytes
       already read into the buffer are served first: poll() does not see
       them. */
    if(!provsql_worker_buffered()) {
      int r = poll(&pfd, 1,
                   store_dirty ? PROVSQL_STORE_FLUSH_INTERVAL_MS : -1);
      if(r < 0) {
        if(errno == EINTR)
          continue;
        break;
      }
      if(r == 0) {
        provsql_store_flush();
        continue;
      }
    }

    provsql_worker_from_spool = provsql_worker_spooled();
    if(!READM(c, char))
      break;
    Oid db_oid, db_tablespace;
    if(!READM(db_oid, Oid) || !READM(db_tablespace, Oid))
      provsql_error("Cannot read message header from pipe");
    provsql_mmap_dispatch(c, db_oid, db_tablespace);
  }

  int e = errno;
  provsql_error("Reading from pipe: %s", strerror(e));
}
#endif

void MMappedCircuit::sync()
{
  gates.sync();
  wires.sync();
  mapping.sync();
  extra.sync();
}

void MMappedCircuit::flush()
{
  /* Order matters as much here as it does on the write path: the records
     a mapping entry points at must be on disk before the entry is, so
     that a machine crash between the two flushes leaves an unreferenced
     record rather than a dangling index. */
  wires.flush();
  extra.flush();
  gates.flush();
  mapping.flush();
}

bool MMappedCircuit::uncleanShutdown() const
{
  return mapping.uncleanShutdown() || gates.uncleanShutdown()
         || wires.uncleanShutdown() || extra.uncleanShutdown();
}

MMappedCircuit::Check MMappedCircuit::check() const
{
  Check c{};
  c.unclean    = uncleanShutdown();
  c.nb_gates   = gates.nbElements();
  c.nb_mapping = mapping.nbElements();
  c.next_value = mapping.nextValue();

  std::vector<bool> referenced(c.nb_gates, false);
  for(unsigned long k=0; k<mapping.capacity(); ++k) {
    unsigned long v = mapping.slotValue(k);
    if(v == MMappedUUIDHashTable::NOTHING)
      continue;
    if(v >= c.nb_gates)
      ++c.dangling_indices;
    else
      referenced[v] = true;
  }
  for(unsigned long i=0; i<c.nb_gates; ++i) {
    if(!referenced[i])
      ++c.unreferenced;
    const GateInformation &gi = gates[i];
    if(gi.children_idx + gi.nb_children > wires.nbElements())
      ++c.bad_wires;
    if(gi.extra_idx + gi.extra_len > extra.nbElements())
      ++c.bad_extra;
  }
  return c;
}


unsigned long MMappedCircuit::mark(const std::vector<pg_uuid_t> &roots,
                                   std::vector<bool> &live) const
{
  const unsigned long n = gates.nbElements();
  live.assign(n, false);
  unsigned long nb_live = 0;

  std::vector<unsigned long> stack;
  for(const auto &r: roots) {
    auto idx = mapping[r];
    if(idx != MMappedUUIDHashTable::NOTHING && idx < n && !live[idx]) {
      live[idx] = true;
      ++nb_live;
      stack.push_back(idx);
    }
  }

  while(!stack.empty()) {
    unsigned long idx = stack.back();
    stack.pop_back();
    const GateInformation &gi = gates[idx];
    if(gi.children_idx + gi.nb_children > wires.nbElements())
      continue;   /* a torn record; check() reports it, do not follow it */
    for(unsigned long k=gi.children_idx; k<gi.children_idx+gi.nb_children; ++k) {
      auto child = mapping[wires[k]];
      if(child != MMappedUUIDHashTable::NOTHING && child < n && !live[child]) {
        live[child] = true;
        ++nb_live;
        stack.push_back(child);
      }
    }
  }

  return nb_live;
}

MMappedCircuit::Counts MMappedCircuit::sweepInto(
  const std::vector<bool> &live,
  const std::string &mp, const std::string &gp,
  const std::string &wp, const std::string &ep) const
{
  Counts out{};
  const unsigned long n = gates.nbElements();

  /* Old index -> new index, assigned in old order so the rewritten store
     keeps the creation order of what it keeps. */
  std::vector<unsigned long> newidx(n, MMappedUUIDHashTable::NOTHING);
  {
    unsigned long next = 0;
    for(unsigned long i=0; i<n; ++i)
      if(live[i])
        newidx[i] = next++;
  }

  const bool legacy = legacyProbabilities();

  {
    MMappedUUIDHashTable nmapping(mp.c_str(), false, MAGIC_MAPPING);
    MMappedVector<GateInformation> ngates(gp.c_str(), false, MAGIC_GATES,
                                          GATES_VERSION);
    MMappedVector<pg_uuid_t> nwires(wp.c_str(), false, MAGIC_WIRES);
    MMappedVector<char> nextra(ep.c_str(), false, MAGIC_EXTRA);

    for(unsigned long i=0; i<n; ++i) {
      if(!live[i])
        continue;
      GateInformation gi = gates[i];

      /* A record whose ranges run past the vectors they index is torn --
         check() counts them -- so copying by those ranges would read off
         the end.  Keep the gate, drop what cannot be read: the rebuilt
         store is then at least internally consistent, which is the point
         of running the clean-up on a damaged store. */
      const unsigned long old_children = gi.children_idx;
      gi.children_idx = nwires.nbElements();
      if(old_children + gi.nb_children > wires.nbElements())
        gi.nb_children = 0;
      for(unsigned c=0; c<gi.nb_children; ++c)
        nwires.add(wires[old_children + c]);

      const unsigned long old_extra = gi.extra_idx;
      if(old_extra + gi.extra_len > extra.nbElements())
        gi.extra_len = 0;
      if(gi.extra_len > 0) {
        gi.extra_idx = nextra.nbElements();
        for(unsigned k=0; k<gi.extra_len; ++k)
          nextra.add(extra[old_extra + k]);
      } else {
        gi.extra_idx = 0;
      }

      /* A version-1 file stored 1.0 both for "written as certain" and for
         "never written"; the rewrite is where that ambiguity is settled,
         conservatively, in favour of unwritten -- the reading the store
         has been giving those gates all along. */
      if(legacy && gi.prob == 1.
         && (gi.type == gate_input || gi.type == gate_update
             || gi.type == gate_mulinput))
        gi.prob = NAN;

      ngates.add(gi);
    }

    for(unsigned long k=0; k<mapping.capacity(); ++k) {
      unsigned long v = mapping.slotValue(k);
      if(v == MMappedUUIDHashTable::NOTHING || v >= n || !live[v])
        continue;
      nmapping.publish(mapping.slotKey(k), newidx[v]);
    }

    out.gates = ngates.nbElements();
    out.wires = nwires.nbElements();
    out.extra_bytes = nextra.nbElements();

    ngates.flush();
    nwires.flush();
    nextra.flush();
    nmapping.flush();
  }

  return out;
}

/**
 * @brief Lexicographic less-than comparison for @c pg_uuid_t.
 * @param a  Left UUID.
 * @param b  Right UUID.
 * @return   @c true if @p a is lexicographically less than @p b.
 */
bool operator<(const pg_uuid_t a, const pg_uuid_t b)
{
  return memcmp(&a, &b, sizeof(pg_uuid_t))<0;
}

GenericCircuit MMappedCircuit::createGenericCircuit(pg_uuid_t token,
                                                    std::size_t budget) const
{
  return createGenericCircuit(std::vector<pg_uuid_t>{token}, budget);
}

GenericCircuit MMappedCircuit::createGenericCircuit(
    const std::vector<pg_uuid_t> &roots, std::size_t budget) const
{
  /* The resident memory the building may add, read every few thousand
   * gates: a circuit of millions of gates is otherwise built, and shipped,
   * whole before anything checks it (GitHub #104). */
  const std::size_t base_rss = budget ? provsql_process_rss(getpid()) : 0;
  unsigned steps = 0;
  /* Counted per gate and per wire: a single gate (the agg of a SUM over
   * millions of rows) can have millions of children. */
  auto check_budget = [&]() {
    if(budget && ++steps % 4096 == 0) {
      std::size_t rss = provsql_process_rss(getpid());
      if(rss > base_rss && rss - base_rss > budget)
        throw CircuitBudgetExceeded();
    }
  };

  /* Seed the work list with every root.  A UUID listed twice (or reached
   * as a child of one root and the other's root itself) is processed only
   * once.  Shared subgraphs therefore land on a single gate_t in `result`
   * -- the property that lets the conditional MC sampler couple the
   * indicator and value paths through @c Sampler::scalar_cache_ /
   * @c bool_cache_.  The smallest UUID waiting is processed first, which
   * fixes the numbering of the gates (a gate is numbered when first met):
   * a min-heap, which a gate enters when first met as a child; a root met
   * so before it is processed is in it twice, and skipped the second
   * time. */
  /* memcmp's order, the UUID read as two big-endian words */
  auto word = [](const unsigned char *p) {
    std::uint64_t w = 0;
    for(int i = 0; i < 8; ++i)
      w = (w << 8) | p[i];
    return w;
  };
  auto greater = [&word](const pg_uuid_t &a, const pg_uuid_t &b) {
    const std::uint64_t a1 = word(a.data), b1 = word(b.data);
    if(a1 != b1)
      return a1 > b1;
    return word(a.data + 8) > word(b.data + 8);
  };
  std::priority_queue<pg_uuid_t, std::vector<pg_uuid_t>, decltype(greater)>
    to_process(greater);
  for(const auto &r : roots)
    to_process.push(r);

  GenericCircuit result;
  /* The gate of each UUID met, without building its string at each wire */
  std::unordered_map<pg_uuid_t, gate_t, UUIDHash> ids;
  std::vector<bool> processed;

  while(!to_process.empty()) {
    check_budget();
    pg_uuid_t uuid = to_process.top();
    to_process.pop();
    auto [it, fresh] = ids.try_emplace(uuid);
    if(fresh)
      it->second = result.getGate(uuid2string(uuid));
    const gate_t id = it->second;
    const std::size_t i = static_cast<std::size_t>(id);
    if(i < processed.size() && processed[i])
      continue;
    if(i >= processed.size())
      processed.resize(std::max<std::size_t>(i + 1, 2 * processed.size()));
    processed[i] = true;

    const unsigned long idx = mapping[uuid];
    copyGateAt(result, id, idx);
    if(idx == MMappedUUIDHashTable::NOTHING)
      continue;

    const GateInformation &gi = gates[idx];
    for(unsigned long k = gi.children_idx; k < gi.children_idx + gi.nb_children;
        ++k) {
      check_budget();
      const pg_uuid_t child = wires[k];
      auto [ic, first] = ids.try_emplace(child);
      if(first) {
        ic->second = result.getGate(uuid2string(child));
        to_process.push(child);
      }
      result.addWire(id, ic->second);
    }
  }

  return result;
}

gate_t MMappedCircuit::copyGate(GenericCircuit &result, pg_uuid_t uuid) const
{
  const gate_t id = result.getGate(uuid2string(uuid));
  copyGateAt(result, id, mapping[uuid]);
  return id;
}

void MMappedCircuit::copyGateAt(GenericCircuit &result, gate_t id,
                                unsigned long idx) const
{
    const bool absent = idx == MMappedUUIDHashTable::NOTHING;
    gate_type type = absent ? gate_input : gates[idx].type;
    result.setGate(id, type);
    double prob = probAt(idx);
    if(!std::isnan(prob))
      result.setProb(id, prob);

    if(type==gate_mulinput || type==gate_eq || type==gate_agg
       || type==gate_cmp  || type==gate_arith || type==gate_fixpoint) {
      const unsigned info1 = absent ? 0 : gates[idx].info1;
      const unsigned info2 = absent ? 0 : gates[idx].info2;
      result.setInfos(id, info1, info2);
    } else if(type==gate_plus || type==gate_times || type==gate_assumed) {
      /* The d-DNNF certificate (DNNF_CERT_INFO in info1: deterministic
       * plus / decomposable times) and, alongside it in info2, the tag of
       * the planner-time route that produced the root (@c provsql_route);
       * on a gate_assumed the route tag is in info1.  Copied only when
       * set, so unmarked gates do not bloat the in-memory infos map with
       * zeros. */
      const unsigned info1 = absent ? 0 : gates[idx].info1;
      const unsigned info2 = absent ? 0 : gates[idx].info2;
      if(info1 != 0 || info2 != 0)
        result.setInfos(id, info1, info2);
    }

    if(type==gate_project || type==gate_value || type==gate_agg
       || type==gate_rv || type==gate_mulinput || type==gate_annotation
       || type==gate_assumed || type==gate_mobius || type==gate_arith
       || type==gate_observe) {
      /* gate_assumed carries its assumption kind ('boolean' /
       * 'absorptive') in extra; gate_mobius carries its per-child integer
       * coefficients ("uuid:coeff" tokens); a gate_arith PERCENTILE
       * carries its fraction (agg-carrier arith gates carry a display
       * value there, inert for evaluation); gates stored without the
       * label have none and default to 'boolean' at
       * evaluation. */
      auto extra = extraAt(idx);
      result.setExtra(id, extra);
    }
}

FormulaRegions MMappedCircuit::formulaRegions(
  const std::vector<FormulaRequest> &requests, unsigned long cap,
  unsigned long wide_cap) const
{
  FormulaRegions r;
  constexpr unsigned long NOTHING = MMappedUUIDHashTable::NOTHING;

  /* The size of the formula of each gate met, by its index, counted up to
   * wide_cap + 1 only: past it, the rest of the children are not looked at.
   * A wire closing a cycle counts for one. */
  std::unordered_map<unsigned long, unsigned long> size;
  struct Frame {
    unsigned long idx;
    unsigned long k;    ///< Next child to count
    unsigned long acc;  ///< Size so far
  };
  auto size_of = [&](unsigned long root) -> unsigned long {
    if(root == NOTHING)
      return 1;
    auto it = size.find(root);
    if(it != size.end())
      return it->second;
    std::unordered_map<unsigned long, bool> on_stack;
    std::vector<Frame> stack{{root, 0, 1}};
    on_stack[root] = true;
    while(!stack.empty()) {
      Frame &f = stack.back();
      const GateInformation &gi = gates[f.idx];
      bool pushed = false;
      /* An equation system is rendered whole: its unknowns have no formula
       * of their own */
      if(gi.type != gate_fixpoint)
        while(f.k < gi.nb_children && f.acc <= wide_cap) {
          const unsigned long c = mapping[wires[gi.children_idx + f.k]];
          unsigned long s;
          if(c == NOTHING)
            s = 1;
          else if(on_stack.count(c))
            s = 1;
          else {
            auto ic = size.find(c);
            if(ic == size.end()) {
              stack.push_back({c, 0, 1});
              on_stack[c] = true;
              pushed = true;
              break;
            }
            s = ic->second;
          }
          f.acc += s > cap ? 1 : s;
          ++f.k;
        }
      if(pushed)
        continue;
      const unsigned long done = std::min(f.acc, wide_cap + 1);
      size[f.idx] = done;
      on_stack.erase(f.idx);
      stack.pop_back();
    }
    return size.at(root);
  };

  std::unordered_set<unsigned long> stubbed;
  std::unordered_set<std::string> included;
  auto stub = [&](pg_uuid_t u, unsigned long idx) {
    if(!stubbed.insert(idx).second)
      return;
    const GateInformation &gi = gates[idx];
    r.circuit.setGate(uuid2string(u), gate_input);
    r.stubs.push_back({uuid2string(u), gi.type, gi.info1, gi.info2,
                       gi.nb_children});
  };
  /* The region of @p u: it, and the gates below down to the named ones,
   * stubs (all of them below an equation system) */
  auto include = [&](pg_uuid_t u) {
    std::vector<std::pair<pg_uuid_t, bool> > todo{{u, false}};
    while(!todo.empty()) {
      auto [v, whole] = todo.back();
      todo.pop_back();
      const std::string vs = uuid2string(v);
      if(!included.insert(vs).second)
        continue;
      const gate_t id = copyGate(r.circuit, v);
      const unsigned long idx = mapping[v];
      if(idx == NOTHING)
        continue;
      const GateInformation &gi = gates[idx];
      whole = whole || gi.type == gate_fixpoint;
      for(unsigned long k = 0; k < gi.nb_children; ++k) {
        const pg_uuid_t w = wires[gi.children_idx + k];
        const unsigned long wi = mapping[w];
        r.circuit.addWire(id, r.circuit.getGate(uuid2string(w)));
        if(!whole && wi != NOTHING && size_of(wi) > cap)
          stub(w, wi);
        else
          todo.push_back({w, whole});
      }
    }
  };

  for(const auto &q : requests) {
    const unsigned long idx = mapping[q.gate];
    if(q.count == 0) {
      if(idx != NOTHING) {
        const GateInformation &gi = gates[idx];
        if(gi.nb_children > 1 && size_of(idx) > wide_cap
           && (gi.type == gate_plus || gi.type == gate_times
               || gi.type == gate_agg)) {
          stub(q.gate, idx);
          continue;
        }
      }
      include(q.gate);
    } else if(idx != NOTHING) {
      const GateInformation &gi = gates[idx];
      for(unsigned long k = q.from; k < q.from + q.count && k < gi.nb_children;
          ++k) {
        const pg_uuid_t w = wires[gi.children_idx + k];
        const unsigned long wi = mapping[w];
        r.children.push_back(uuid2string(w));
        if(wi != NOTHING && size_of(wi) > cap)
          stub(w, wi);
        else
          include(w);
      }
    }
  }

  return r;
}
