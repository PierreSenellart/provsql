#include <cerrno>
#include <cmath>

#include "MMappedCircuit.h"

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
    if(c=='C') {
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
    } else if(c=='P') {
      pg_uuid_t token;
      double prob;

      if(!READM(token, pg_uuid_t) || !READM(prob, double))
        elog(ERROR, "Cannot read from pipe (message type P)");

      circuit->setProb(token, prob);
    } else if(c=='I') {
      pg_uuid_t token;
      int info1, info2;

      if(!READM(token, pg_uuid_t) || !READM(info1, int) || !READM(info2, int))
        elog(ERROR, "Cannot read from pipe (message type I)");

      circuit->setInfos(token, info1, info2);
    } else if(c=='t') {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type t)");

      gate_type type = circuit->getGateType(token);

      if(!WRITEB(&type, gate_type))
        elog(ERROR, "Cannot write response to pipe (message type t)");
    } else if(c=='n') {
      unsigned nb = circuit->getNbGates();

      if(!WRITEB(&nb, unsigned))
        elog(ERROR, "Cannot write response to pipe (message type n)");
    } else if(c=='c') {
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
    } else if(c=='p') {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type p)");

      double prob = circuit->getProb(token);

      if(!WRITEB(&prob, double))
        elog(ERROR, "Cannot write response to pipe (message type p)");
    } else if(c=='i') {
      pg_uuid_t token;

      if(!READM(token, pg_uuid_t))
        elog(ERROR, "Cannot read from pipe (message type i)");

      auto infos = circuit->getInfos(token);

      if(!WRITEB(&infos.first, int) || !WRITEB(&infos.second, int))
        elog(ERROR, "Cannot write response to pipe (message type i)");
    } else {
      elog(ERROR, "Wrong message type: %c", c);
    }
  }

  int e = errno;
  elog(ERROR, "Reading from pipe: %s", strerror(e));
}
