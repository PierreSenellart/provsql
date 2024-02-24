#include <cerrno>

#include "MMappedCircuit.h"

extern "C" {
#include "provsql_mmap.h"
#include "provsql_shmem.h"
}

static MMappedCircuit *circuit;

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
    return -1;
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

#define READ(var, type) !(read(provsql_shared_state->piper, &var, sizeof(type))-sizeof(type)<0) // flawfinder: ignore

void provsql_mmap_main_loop()
{
  char c;

  while(READ(c, char)) {
    if(c=='C') {
      pg_uuid_t token;
      gate_type type;
      unsigned nb_children;

      if(!READ(token, pg_uuid_t) || !READ(type, gate_type) || !READ(nb_children, unsigned))
        elog(ERROR, "Cannot read from pipe (message type C)"); ;

      std::vector<pg_uuid_t> children(nb_children);
      for(unsigned i=0; i<nb_children; ++i)
        if(!READ(children[i], pg_uuid_t))
          elog(ERROR, "Cannot read from pipe (message type C)");

//      elog(LOG, "Adding gate to circuit with %d children", nb_children);
      circuit->createGate(token, type, children);
    } else if(c=='P') {
      pg_uuid_t token;
      double prob;

      if(!READ(token, pg_uuid_t) || !READ(prob, double))
        elog(ERROR, "Cannot read from pipe (message type P)");

//      elog(LOG, "Setting probability to %g", prob);
      circuit->setProb(token, prob);
    } else if(c=='I') {
      pg_uuid_t token;
      int info1, info2;

      if(!READ(token, pg_uuid_t) || !READ(info1, int) || !READ(info2, int))
        elog(ERROR, "Cannot read from pipe (message type I)");

//      elog(LOG, "Setting infos to %d %d", info1, info2);
      circuit->setInfos(token, info1, info2);
    } else {
      elog(ERROR, "Wrong message type: %c", c);
    }
  }

  int e = errno;
  elog(ERROR, "Reading from pipe: %s", strerror(e));
}
