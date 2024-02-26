#include <cerrno>
#include <cmath>
#include <sstream>

#include "MMappedCircuit.h"
#include "BooleanCircuit.h"
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
    for(unsigned k=gi.children_idx; k<gi.children_idx+gi.nb_children; ++k)
      result.push_back(wires[k]);
  }
  return result;
}

void MMappedCircuit::setProb(pg_uuid_t token, double prob)
{
  auto idx = mapping[token];
  if(idx != MMappedUUIDHashTable::NOTHING)
    gates[idx].prob=prob;
}

double MMappedCircuit::getProb(pg_uuid_t token) const
{
  auto idx = mapping[token];
  if(idx == MMappedUUIDHashTable::NOTHING)
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

      circuit->setProb(token, prob);
      break;
    }

    case 'I':
    {
      pg_uuid_t token;
      int info1, info2;

      if(!READM(token, pg_uuid_t) || !READM(info1, int) || !READM(info2, int))
        elog(ERROR, "Cannot read from pipe (message type I)");

      circuit->setInfos(token, info1, info2);
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
      unsigned nb = circuit->getNbGates();

      if(!WRITEB(&nb, unsigned))
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

      for(unsigned i=0; i<nb_children; ++i)
        if(!WRITEB(&children[i], pg_uuid_t))
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

      if(!WRITEB(&infos.first, int) || !WRITEB(&infos.second, int))
        elog(ERROR, "Cannot write response to pipe (message type i)");
      break;
    }

    case 'g':
    {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type g)");

      std::stringstream ss;
      boost::archive::binary_oarchive oa(ss);
      oa << createBooleanCircuit(token);

      ss.seekg(0, std::ios::end);
      unsigned size = ss.tellg();
      ss.seekg(0, std::ios::beg);

      if(!WRITEB(&size, unsigned) || write(provsql_shared_state->pipembw, ss.str().data(), size)==-1)
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
