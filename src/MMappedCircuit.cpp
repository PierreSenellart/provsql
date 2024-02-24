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

void provsql_mmap_main_loop()
{
  char c;
  ssize_t ret;

  while((ret=read(provsql_shared_state->piper, &c, 1))>0)     // flawfinder: ignore
    if(c=='C') {
      pg_uuid_t token;
      gate_type type;
      unsigned nb_children;

      if(read(provsql_shared_state->piper, &token, sizeof(pg_uuid_t))<sizeof(pg_uuid_t)
         || read(provsql_shared_state->piper, &type, sizeof(gate_type))<sizeof(gate_type)
         || read(provsql_shared_state->piper, &nb_children, sizeof(unsigned))<sizeof(unsigned))
        elog(ERROR, "Cannot read from pipe"); ;

      std::vector<pg_uuid_t> children(nb_children);
      for(unsigned i=0; i<nb_children; ++i)
        if(read(provsql_shared_state->piper, &children[i], sizeof(pg_uuid_t))<sizeof(pg_uuid_t))
          elog(ERROR, "Cannot read from pipe");

      elog(LOG, "Adding gate to circuit with %d children", nb_children);
      circuit->createGate(token, type, children);
    } else {
      elog(ERROR, "Wrong message type: %c", c);
    }
  elog(LOG, "Read from pipe: %c", c);

  if(ret<0) {
    int e = errno;
    elog(ERROR, "Reading from pipe: %s", strerror(e));
  }
}
