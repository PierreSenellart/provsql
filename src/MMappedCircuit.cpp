/**
 * @file MMappedCircuit.cpp
 * @brief Persistent mmap-backed circuit: implementation and background-worker entry points.
 *
 * Implements the @c MMappedCircuit methods declared in @c MMappedCircuit.h,
 * the @c createGenericCircuit() free function, and the background-worker
 * entry points declared in @c provsql_mmap.h:
 *
 * - @c initialize_provsql_mmap(): called by the background worker at
 *   startup; opens all four mmap files and creates the singleton
 *   @c MMappedCircuit instance.
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
#include <sstream>
#include <string>

#include "MMappedCircuit.h"
#include "GenericCircuit.h"
#include "Circuit.hpp"
#include "provsql_utils_cpp.h"

extern "C" {
#include "miscadmin.h"
#include "provsql_mmap.h"
#include "provsql_shmem.h"
}

/** @brief Per-database mmap-backed provenance circuits, keyed by database OID. */
static std::map<Oid, MMappedCircuit*> circuits;

std::string MMappedCircuit::makePath(Oid db_oid, const char *filename)
{
  return std::string(DataDir) + "/base/" + std::to_string(db_oid) + "/" + filename;
}

MMappedCircuit::MMappedCircuit(Oid db_oid, bool read_only) :
  MMappedCircuit(
    makePath(db_oid, MAPPING_FILENAME),
    makePath(db_oid, GATES_FILENAME),
    makePath(db_oid, WIRES_FILENAME),
    makePath(db_oid, EXTRA_FILENAME),
    makePath(db_oid, TABLE_INFO_FILENAME),
    read_only) {}

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

void MMappedCircuit::createGate(
  pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children)
{
  auto [idx, created] = mapping.add(token);
  if(!created) {
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
      gates[idx].type = type;
      gates[idx].nb_children = static_cast<unsigned>(children.size());
      gates[idx].children_idx = wires.nbElements();
      for(const auto &c: children)
        wires.add(c);
      for(const auto &c: children) {
        auto [child_idx, child_created] = mapping.add(c);
        if(child_created)
          gates.add({gate_input, 0, wires.nbElements()});
      }
    }
    return;
  }

  gates.add({type, static_cast<unsigned>(children.size()), wires.nbElements()});
  for(const auto &c: children)
    wires.add(c);

  for(const auto &c: children) {
    auto [child_idx, child_created] = mapping.add(c);
    if(child_created)
      gates.add({gate_input, 0, wires.nbElements()});
  }
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

bool MMappedCircuit::setProb(pg_uuid_t token, double prob)
{
  auto [idx, created] = mapping.add(token);
  if(created)
    gates.add({gate_input, 0, wires.nbElements()});
  if(gates[idx].type == gate_input || gates[idx].type == gate_update || gates[idx].type == gate_mulinput) {
    gates[idx].prob = prob;
    return true;
  }
  return false;
}

double MMappedCircuit::getProb(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING ||
     (gates[idx].type != gate_input && gates[idx].type != gate_update && gates[idx].type != gate_mulinput))
    return NAN;
  else
    return gates[idx].prob;
}

void MMappedCircuit::setInfos(pg_uuid_t token, unsigned info1, unsigned info2)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    gates[idx].info1=info1;
    gates[idx].info2=info2;
  }
}

void MMappedCircuit::setExtra(pg_uuid_t token, const std::string &s)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    gates[idx].extra_idx=extra.nbElements();
    for(auto c: s)
      extra.add(c);
    gates[idx].extra_len=s.size();
  }
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
  std::string result;

  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING) {
    for(unsigned long start=gates[idx].extra_idx, k=start, end=start+gates[idx].extra_len; k<end; ++k)
      result+=extra[k];
  }

  return result;
}

/** @brief Return (creating lazily if needed) the circuit for @p db_oid. */
static MMappedCircuit *getCircuit(Oid db_oid)
{
  auto it = circuits.find(db_oid);
  if(it == circuits.end()) {
    circuits[db_oid] = new MMappedCircuit(db_oid);
    return circuits[db_oid];
  }
  return it->second;
}

#ifdef PROVSQL_INPROCESS_STORE
/* Single-process build: the backend builds the in-memory circuit directly
   from this process's store, instead of round-tripping a Boost-serialised
   copy through the FIFO (which only existed to cross the worker/backend
   process boundary).  This is also what lets the WASM build avoid the
   compiled libboost_serialization dependency. */
GenericCircuit provsql_inproc_generic_circuit(pg_uuid_t token)
{
  return getCircuit(MyDatabaseId)->createGenericCircuit(token);
}

GenericCircuit provsql_inproc_joint_circuit(pg_uuid_t root, pg_uuid_t event)
{
  return getCircuit(MyDatabaseId)->createGenericCircuit(
    std::vector<pg_uuid_t>{root, event});
}
#endif

extern "C" void provsql_mmap_dispatch(char c, Oid db_oid)
{
    MMappedCircuit *circuit = getCircuit(db_oid);

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

    case 'P':
    {
      pg_uuid_t token;
      double prob;

      if(!READM(token, pg_uuid_t) || !READM(prob, double))
        provsql_error("Cannot read from pipe (message type P)");

      bool ok = circuit->setProb(token, prob);
      char return_value = ok?static_cast<char>(1):0;

      if(!WRITEB(&return_value, char))
        provsql_error("Cannot write response to pipe (message type P)");
      break;
    }

    case 'I':
    {
      pg_uuid_t token;
      unsigned info1, info2;

      if(!READM(token, pg_uuid_t) || !READM(info1, unsigned) || !READM(info2, unsigned))
        provsql_error("Cannot read from pipe (message type I)");

      circuit->setInfos(token, info1, info2);
      break;
    }

    case 'E':
    {
      pg_uuid_t token;
      unsigned len;

      if(!READM(token, pg_uuid_t) || !READM(len, unsigned))
        provsql_error("Cannot read from pipe (message type E)");

      if(len>0) {
        char *data = new char[len];
        if(!READM_BYTES(data, len))
          provsql_error("Cannot read from pipe (message type E)");

        circuit->setExtra(token, std::string(data, len));
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

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type g)");

#ifdef PROVSQL_INPROCESS_STORE
      /* Unreachable: the backend calls provsql_inproc_generic_circuit
         directly instead of issuing the 'g' message. */
      provsql_error("message type g is not used by the in-process store");
#else
      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << circuit->createGenericCircuit(token);

      ss.seekg(0, std::ios::end);
      unsigned long size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned long) || !WRITEB_BYTES(ss.str().data(), size))
        provsql_error("Cannot write to pipe (message type g)");
#endif
      break;
    }

    case 'T':
    {
      /* Insert / upsert per-table provenance metadata. */
      ProvenanceTableInfo info{};
      if(!READM(info.relid, Oid) || !READM(info.kind, uint8_t)
         || !READM(info.block_key_n, uint16_t))
        provsql_error("Cannot read from pipe (message type T)");
      if(info.block_key_n > PROVSQL_TABLE_INFO_MAX_BLOCK_KEY)
        provsql_error("ProvSQL: block key wider than %d columns "
                      "(message type T)", PROVSQL_TABLE_INFO_MAX_BLOCK_KEY);
      for(uint16_t i=0; i<info.block_key_n; ++i)
        if(!READM(info.block_key[i], AttrNumber))
          provsql_error("Cannot read from pipe (message type T)");
      circuit->setTableInfo(info);
      break;
    }

    case 'D':
    {
      /* Delete per-table provenance metadata. */
      Oid relid;
      if(!READM(relid, Oid))
        provsql_error("Cannot read from pipe (message type D)");
      circuit->removeTableInfo(relid);
      break;
    }

    case 's':
    {
      /* Look up per-table provenance metadata. */
      Oid relid;
      if(!READM(relid, Oid))
        provsql_error("Cannot read from pipe (message type s)");
      ProvenanceTableInfo info{};
      char found = circuit->getTableInfo(relid, info) ? 1 : 0;
      if(!WRITEB(&found, char))
        provsql_error("Cannot write response to pipe (message type s)");
      if(found) {
        if(!WRITEB(&info.kind, uint8_t) || !WRITEB(&info.block_key_n, uint16_t))
          provsql_error("Cannot write response to pipe (message type s)");
        for(uint16_t i=0; i<info.block_key_n; ++i)
          if(!WRITEB(&info.block_key[i], AttrNumber))
            provsql_error("Cannot write response to pipe (message type s)");
      }
      break;
    }

    case 'A':
    {
      /* Insert / upsert the ancestor half of a per-table metadata
       * record (the kind / block_key half is preserved). */
      Oid relid;
      uint16_t ancestor_n;
      if(!READM(relid, Oid) || !READM(ancestor_n, uint16_t))
        provsql_error("Cannot read from pipe (message type A)");
      if(ancestor_n > PROVSQL_TABLE_INFO_MAX_ANCESTORS)
        provsql_error("ProvSQL: ancestor set wider than %d entries "
                      "(message type A)",
                      PROVSQL_TABLE_INFO_MAX_ANCESTORS);
      Oid ancestors[PROVSQL_TABLE_INFO_MAX_ANCESTORS];
      for(uint16_t i=0; i<ancestor_n; ++i)
        if(!READM(ancestors[i], Oid))
          provsql_error("Cannot read from pipe (message type A)");
      circuit->setTableAncestry(relid, ancestor_n, ancestors);
      break;
    }

    case 'R':
    {
      /* Clear just the ancestor half of a per-table metadata record. */
      Oid relid;
      if(!READM(relid, Oid))
        provsql_error("Cannot read from pipe (message type R)");
      circuit->removeTableAncestry(relid);
      break;
    }

    case 'a':
    {
      /* Look up just the ancestor half of a per-table metadata record. */
      Oid relid;
      if(!READM(relid, Oid))
        provsql_error("Cannot read from pipe (message type a)");
      ProvenanceTableInfo info{};
      char found = circuit->getTableInfo(relid, info) ? 1 : 0;
      if(!WRITEB(&found, char))
        provsql_error("Cannot write response to pipe (message type a)");
      if(found) {
        if(!WRITEB(&info.ancestor_n, uint16_t))
          provsql_error("Cannot write response to pipe (message type a)");
        for(uint16_t i=0; i<info.ancestor_n; ++i)
          if(!WRITEB(&info.ancestors[i], Oid))
            provsql_error("Cannot write response to pipe (message type a)");
      }
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

#ifdef PROVSQL_INPROCESS_STORE
      /* Unreachable: the backend calls provsql_inproc_joint_circuit
         directly instead of issuing the 'j' message. */
      provsql_error("message type j is not used by the in-process store");
#else
      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << circuit->createGenericCircuit(roots);

      ss.seekg(0, std::ios::end);
      unsigned long size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned long) || !WRITEB_BYTES(ss.str().data(), size))
        provsql_error("Cannot write to pipe (message type j)");
#endif
      break;
    }

    default:
      provsql_error("Wrong message type: %c", c);
    }
}

#ifndef PROVSQL_INPROCESS_STORE
void provsql_mmap_main_loop()
{
  char c;

  while(READM(c, char)) {
    Oid db_oid;
    if(!READM(db_oid, Oid))
      provsql_error("Cannot read database OID from pipe");
    provsql_mmap_dispatch(c, db_oid);
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
  tableInfo.sync();
}

/* The tableInfo vector uses a tombstone scheme: removed entries have
 * their relid set to InvalidOid and remain in place.  setTableInfo()
 * reuses tombstone slots before appending.  All readers skip
 * InvalidOid entries.  This avoids reaching into MMappedVector's
 * append-only public API, and keeps the file format trivial: a
 * crash-recovered file is internally consistent without any extra
 * recovery step.  In practice, churn on this vector is low (one
 * entry per add_provenance / repair_key / remove_provenance call).
 *
 * Each record carries two logically independent halves: the kind /
 * block_key fields (TID / BID classification, set by add_provenance /
 * repair_key / set_table_info) and the ancestor_n / ancestors fields
 * (base-relation provenance lineage, set by add_provenance for base
 * tables and by the CTAS hook for derived tables).  setTableInfo()
 * updates the kind half and preserves the ancestor half on update;
 * setTableAncestry() does the converse.  This lets the two halves
 * evolve independently without the SQL layer having to fetch-and-
 * round-trip every time. */

void MMappedCircuit::setTableInfo(const ProvenanceTableInfo &info)
{
  long tombstone = -1;
  unsigned long n = tableInfo.nbElements();
  for(unsigned long i=0; i<n; ++i) {
    if(tableInfo[i].relid == info.relid) {
      /* Preserve the existing ancestor half on update. */
      ProvenanceTableInfo merged = info;
      merged.ancestor_n = tableInfo[i].ancestor_n;
      memcpy(merged.ancestors, tableInfo[i].ancestors,
             merged.ancestor_n * sizeof(Oid));
      tableInfo[i] = merged;
      return;
    }
    if(tombstone < 0 && tableInfo[i].relid == InvalidOid)
      tombstone = static_cast<long>(i);
  }
  /* Fresh record: kind half from caller, ancestor half empty. */
  ProvenanceTableInfo fresh = info;
  fresh.ancestor_n = 0;
  if(tombstone >= 0)
    tableInfo[tombstone] = fresh;
  else
    tableInfo.add(fresh);
}

void MMappedCircuit::setTableAncestry(Oid relid, uint16_t ancestor_n,
                                      const Oid *ancestors)
{
  if(relid == InvalidOid)
    return;
  if(ancestor_n > PROVSQL_TABLE_INFO_MAX_ANCESTORS)
    return;  /* defensive: caller-side check already rejects this */
  unsigned long n = tableInfo.nbElements();
  for(unsigned long i=0; i<n; ++i) {
    if(tableInfo[i].relid == relid) {
      ProvenanceTableInfo updated = tableInfo[i];
      updated.ancestor_n = ancestor_n;
      memcpy(updated.ancestors, ancestors, ancestor_n * sizeof(Oid));
      tableInfo[i] = updated;
      return;
    }
  }
  /* No-op when relid has no kind record: callers should set kind
   * first (add_provenance / repair_key do this).  Silently
   * dropping the ancestry payload here matches the existing
   * removeTableInfo / setTableInfo "missing relid is harmless"
   * pattern and avoids creating an OPAQUE-by-default record. */
}

void MMappedCircuit::removeTableInfo(Oid relid)
{
  if(relid == InvalidOid)
    return;
  unsigned long n = tableInfo.nbElements();
  for(unsigned long i=0; i<n; ++i) {
    if(tableInfo[i].relid == relid) {
      tableInfo[i].relid = InvalidOid;
      return;
    }
  }
}

void MMappedCircuit::removeTableAncestry(Oid relid)
{
  if(relid == InvalidOid)
    return;
  unsigned long n = tableInfo.nbElements();
  for(unsigned long i=0; i<n; ++i) {
    if(tableInfo[i].relid == relid) {
      tableInfo[i].ancestor_n = 0;
      return;
    }
  }
}

bool MMappedCircuit::getTableInfo(Oid relid, ProvenanceTableInfo &out) const
{
  if(relid == InvalidOid)
    return false;
  for(unsigned long i=0; i<tableInfo.nbElements(); ++i) {
    if(tableInfo[i].relid == relid) {
      out = tableInfo[i];
      return true;
    }
  }
  return false;
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

GenericCircuit MMappedCircuit::createGenericCircuit(pg_uuid_t token) const
{
  return createGenericCircuit(std::vector<pg_uuid_t>{token});
}

GenericCircuit MMappedCircuit::createGenericCircuit(
    const std::vector<pg_uuid_t> &roots) const
{
  /* Seed the work list with every root.  std::set deduplicates so a
   * UUID listed twice (or reached as a child of one root and the
   * other's root itself) is processed only once.  Shared subgraphs
   * therefore land on a single gate_t in `result` -- the property
   * that lets the conditional MC sampler couple the indicator and
   * value paths through @c Sampler::scalar_cache_ / @c bool_cache_. */
  std::set<pg_uuid_t> to_process, processed;
  for(const auto &r : roots)
    to_process.insert(r);

  GenericCircuit result;

  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    gate_type type = getGateType(uuid);
    gate_t id = result.setGate(f, type);
    double prob = getProb(uuid);
    if(!std::isnan(prob))
      result.setProb(id, prob);

    std::vector<pg_uuid_t> children = getChildren(uuid);
    for(unsigned i=0; i<children.size(); ++i) {
      result.addWire(
        id,
        result.getGate(uuid2string(children[i])));
      if(processed.find(children[i])==processed.end())
        to_process.insert(children[i]);
    }

    if(type==gate_mulinput || type==gate_eq || type==gate_agg
       || type==gate_cmp  || type==gate_arith) {
      auto [info1, info2] = getInfos(uuid);
      result.setInfos(id, info1, info2);
    } else if(type==gate_plus || type==gate_times) {
      /* The d-DNNF certificate (DNNF_CERT_INFO in info1: deterministic
       * plus / decomposable times).  Copied only when set, so unmarked
       * gates do not bloat the in-memory infos map with zeros. */
      auto [info1, info2] = getInfos(uuid);
      if(info1 != 0 || info2 != 0)
        result.setInfos(id, info1, info2);
    }

    if(type==gate_project || type==gate_value || type==gate_agg
       || type==gate_rv || type==gate_mulinput || type==gate_annotation
       || type==gate_assumed) {
      /* gate_assumed carries its assumption kind ('boolean' /
       * 'absorptive') in extra; gates from stores predating the label
       * have none and default to the historical 'boolean' at
       * evaluation. */
      auto extra = getExtra(uuid);
      result.setExtra(id, extra);
    }
  }

  return result;
}
