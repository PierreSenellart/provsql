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
  if(!created) // Was already existing, no need to do anything
    return;

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

void provsql_mmap_main_loop()
{
  char c;

  while(READM(c, char)) {
    Oid db_oid;
    if(!READM(db_oid, Oid))
      provsql_error("Cannot read database OID from pipe");

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
        if(read(provsql_shared_state->pipebmr, data, len)<len)
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

      if(write(provsql_shared_state->pipembw, &children[0], nb_children*sizeof(pg_uuid_t))==-1)
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

      if(!WRITEB(&len, unsigned) || write(provsql_shared_state->pipembw, str.data(), len)==-1)
        provsql_error("Cannot write response to pipe (message type e)");
      break;
    }

    case 'g':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        provsql_error("Cannot read from pipe (message type g)");

      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << circuit->createGenericCircuit(token);

      ss.seekg(0, std::ios::end);
      unsigned long size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned long) || write(provsql_shared_state->pipembw, ss.str().data(), size)==-1)
        provsql_error("Cannot write to pipe (message type g)");
      break;
    }

    default:
      provsql_error("Wrong message type: %c", c);
    }
  }

  int e = errno;
  provsql_error("Reading from pipe: %s", strerror(e));
}

void MMappedCircuit::sync()
{
  gates.sync();
  wires.sync();
  mapping.sync();
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
  std::set<pg_uuid_t> to_process, processed;
  to_process.insert(token);

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

    if(type==gate_mulinput || type==gate_eq || type==gate_agg || type==gate_cmp) {
      auto [info1, info2] = getInfos(uuid);
      result.setInfos(id, info1, info2);
    }

    if(type==gate_project || type==gate_value || type==gate_agg) {
      auto extra = getExtra(uuid);
      result.setExtra(id, extra);
    }
  }

  return result;
}
