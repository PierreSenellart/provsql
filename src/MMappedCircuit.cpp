#include <cerrno>
#include <cmath>
#include <sstream>

#include "MMappedCircuit.h"
#include "BooleanCircuit.h"
#include "GenericCircuit.h"
#include "Circuit.hpp"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_mmap.h"
#include "provsql_shmem.h"
}

static MMappedCircuit *circuit = NULL;

void initialize_provsql_mmap()
{
  circuit = new MMappedCircuit();
}

void destroy_provsql_mmap()
{
  delete circuit;
}

void MMappedCircuit::createGate(
  pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children)
{
  auto [idx, created] = mapping.add(token);
  if(!created) // Was already existing, no need to do anything
    return;

  gates.add({type, static_cast<unsigned>(children.size()), wires.nbElements()});
  for(const auto &c: children)
  {
    wires.add(c);
  }
}

gate_type MMappedCircuit::getGateType(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
    return gate_invalid;
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
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING &&
     (gates[idx].type == gate_input || gates[idx].type==gate_update || gates[idx].type == gate_mulinput)) {
    gates[idx].prob=prob;
    return true;
  } else
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

void provsql_mmap_main_loop()
{
  char c;

  while(READM(c, char)) {
    switch(c) {
    case 'C':
    {
      pg_uuid_t token;
      gate_type type;
      unsigned nb_children;

      if(!READM(token, pg_uuid_t) || !READM(type, gate_type) || !READM(nb_children, unsigned))
        elog(ERROR, "Cannot read from pipe (message type C)"); ;

      std::vector<pg_uuid_t> children(nb_children);
      for(unsigned i=0; i<nb_children; ++i)
        if(!READM(children[i], pg_uuid_t))
          elog(ERROR, "Cannot read from pipe (message type C)");

      circuit->createGate(token, type, children);
      break;
    }

    case 'P':
    {
      pg_uuid_t token;
      double prob;

      if(!READM(token, pg_uuid_t) || !READM(prob, double))
        elog(ERROR, "Cannot read from pipe (message type P)");

      bool ok = circuit->setProb(token, prob);
      char return_value = ok?static_cast<char>(1):0;

      if(!WRITEB(&return_value, char))
        elog(ERROR, "Cannot write response to pipe (message type P)");
      break;
    }

    case 'I':
    {
      pg_uuid_t token;
      unsigned info1, info2;

      if(!READM(token, pg_uuid_t) || !READM(info1, unsigned) || !READM(info2, unsigned))
        elog(ERROR, "Cannot read from pipe (message type I)");

      circuit->setInfos(token, info1, info2);
      break;
    }

    case 'E':
    {
      pg_uuid_t token;
      unsigned len;

      if(!READM(token, pg_uuid_t) || !READM(len, unsigned))
        elog(ERROR, "Cannot read from pipe (message type E)");

      if(len>0) {
        char *data = new char[len];
        if(read(provsql_shared_state->pipebmr, data, len)<len)
          elog(ERROR, "Cannot read from pipe (message type E)");

        circuit->setExtra(token, std::string(data, len));
      }

      break;
    }

    case 't':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type t)");

      gate_type type = circuit->getGateType(token);

      if(!WRITEB(&type, gate_type))
        elog(ERROR, "Cannot write response to pipe (message type t)");
      break;
    }

    case 'n':
    {
      unsigned long nb = circuit->getNbGates();

      if(!WRITEB(&nb, unsigned long))
        elog(ERROR, "Cannot write response to pipe (message type n)");
      break;
    }

    case 'c':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type c)");

      auto children = circuit->getChildren(token);
      unsigned nb_children = children.size();
      if(!WRITEB(&nb_children, unsigned))
        elog(ERROR, "Cannot write response to pipe (message type c)");

      if(write(provsql_shared_state->pipembw, &children[0], nb_children*sizeof(pg_uuid_t))==-1)
        elog(ERROR, "Cannot write response to pipe (message type c)");
      break;
    }

    case 'p':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type p)");

      double prob = circuit->getProb(token);

      if(!WRITEB(&prob, double))
        elog(ERROR, "Cannot write response to pipe (message type p)");
      break;
    }

    case 'i':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type i)");

      auto infos = circuit->getInfos(token);

      if(!WRITEB(&infos.first, unsigned) || !WRITEB(&infos.second, unsigned))
        elog(ERROR, "Cannot write response to pipe (message type i)");
      break;
    }

    case 'e':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type e)");

      auto str = circuit->getExtra(token);
      unsigned len = str.size();

      if(!WRITEB(&len, unsigned) || write(provsql_shared_state->pipembw, str.data(), len)==-1)
        elog(ERROR, "Cannot write response to pipe (message type e)");
      break;
    }

    case 'b':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type b)");

      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << createBooleanCircuit(token);

      ss.seekg(0, std::ios::end);
      unsigned long size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned long) || write(provsql_shared_state->pipembw, ss.str().data(), size)==-1)
        elog(ERROR, "Cannot write to pipe (message type b)");
      break;
    }

    case 'g':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type g)");

      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << createGenericCircuit(token);

      ss.seekg(0, std::ios::end);
      unsigned long size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned long) || write(provsql_shared_state->pipembw, ss.str().data(), size)==-1)
        elog(ERROR, "Cannot write to pipe (message type g)");
      break;
    }

    default:
      elog(ERROR, "Wrong message type: %c", c);
    }
  }

  int e = errno;
  elog(ERROR, "Reading from pipe: %s", strerror(e));
}

void MMappedCircuit::sync()
{
  gates.sync();
  wires.sync();
  mapping.sync();
}

bool operator<(const pg_uuid_t a, const pg_uuid_t b)
{
  return memcmp(&a, &b, sizeof(pg_uuid_t))<0;
}

BooleanCircuit createBooleanCircuit(pg_uuid_t token)
{
  std::set<pg_uuid_t> to_process, processed;
  to_process.insert(token);

  BooleanCircuit result;

  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    gate_t id;

    gate_type type = circuit->getGateType(uuid);
    double prob = circuit->getProb(uuid);
    auto [info1, info2] = circuit->getInfos(uuid);
    std::vector<pg_uuid_t> children = circuit->getChildren(uuid);

    switch(type) {
    case gate_input:
    case gate_update:
      if(std::isnan(prob)) {
        prob=1.;
      }
      id = result.setGate(f, BooleanGate::IN, prob);
      break;

    case gate_mulinput:
      if(std::isnan(prob)) {
        elog(ERROR, "Missing probability for mulinput token");
      }
      id = result.setGate(f, BooleanGate::MULIN, prob);
      result.addWire(
        id,
        result.getGate(uuid2string(children[0])));
      result.setInfo(id, info1);
      break;

    case gate_times:
    case gate_project:
    case gate_eq:
    case gate_monus:
    case gate_one:
    case gate_delta:
      id = result.setGate(f, BooleanGate::AND);
      break;

    case gate_plus:
    case gate_zero:
      id = result.setGate(f, BooleanGate::OR);
      break;

    default:
      elog(ERROR, "Wrong type of gate in circuit");
    }

    if(children.size() > 0 && type != gate_mulinput) {
      if(type == gate_monus) {
        auto id_not = result.setGate(BooleanGate::NOT);
        result.addWire(
          id,
          result.getGate(uuid2string(children[0])));
        result.addWire(id, id_not);
        result.addWire(
          id_not,
          result.getGate(uuid2string(children[1])));
        if(processed.find(children[0])==processed.end())
          to_process.insert(children[0]);
        if(processed.find(children[1])==processed.end())
          to_process.insert(children[1]);
      } else {
        for(unsigned i=0; i<children.size(); ++i) {
          result.addWire(
            id,
            result.getGate(uuid2string(children[i])));
          if(processed.find(children[i])==processed.end())
            to_process.insert(children[i]);
        }
      }
    }
  }

  return result;
}

GenericCircuit createGenericCircuit(pg_uuid_t token)
{
  std::set<pg_uuid_t> to_process, processed;
  to_process.insert(token);

  GenericCircuit result;

  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    gate_type type = circuit->getGateType(uuid);
    gate_t id = result.setGate(f, type);

    std::vector<pg_uuid_t> children = circuit->getChildren(uuid);
    for(unsigned i=0; i<children.size(); ++i) {
      result.addWire(
        id,
        result.getGate(uuid2string(children[i])));
      if(processed.find(children[i])==processed.end())
        to_process.insert(children[i]);
    }

    if(type==gate_mulinput || type==gate_eq || type==gate_agg || type==gate_cmp) {
      auto [info1, info2] = circuit->getInfos(uuid);
      result.setInfos(id, info1, info2);
    }

    if(type==gate_project || type==gate_value || type==gate_agg) {
      auto extra = circuit->getExtra(uuid);
      result.setExtra(id, extra);
    }
  }

  return result;
}
