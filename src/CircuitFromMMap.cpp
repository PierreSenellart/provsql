#include <cmath>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#include "CircuitFromMMap.h"
#include "having_semantics.hpp"
#include "semiring/BoolExpr.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_shmem.h"
#include "provsql_mmap.h"
}

template<typename C>
static C getCircuitFromMMap(pg_uuid_t token, char message_char)
{
  provsql_shmem_lock_exclusive();
  if(!WRITEM(&message_char, char) || !WRITEM(&token, pg_uuid_t))
    elog(ERROR, "Cannot write to pipe (message type %c)", message_char);

  unsigned long size;
  if(!READB(size, unsigned long))
    elog(ERROR, "Cannot read from pipe (message type %c)", message_char);

  char *buf = new char[size], *p = buf;
  ssize_t actual_read, remaining_size=size;
  while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
    if(actual_read<=0) {
      provsql_shmem_unlock();
      delete [] buf;
      elog(ERROR, "Cannot read from pipe (message type %c)", message_char);
    } else {
      remaining_size-=actual_read;
      p+=actual_read;
    }
  }
  provsql_shmem_unlock();

  boost::iostreams::stream<boost::iostreams::array_source> stream(buf, size);
  boost::archive::binary_iarchive ia(stream);
  C c;
  ia >> c;

  delete [] buf;

  return c;
}

BooleanCircuit getBooleanCircuit(pg_uuid_t token, gate_t &gate)
{
  GenericCircuit gc = getGenericCircuit(token);
  auto ggate = gc.getGate(uuid2string(token));
  BooleanCircuit c;
  std::unordered_map<gate_t, gate_t> mapping;
  for(gate_t u: gc.getInputs()) {
    mapping[u]=c.setGate(gc.getUUID(u), BooleanGate::IN, gc.getProb(u));
  }
  for(size_t i=0; i<gc.getNbGates(); ++i) {
    auto u=static_cast<gate_t>(i);
    if(gc.getGateType(u)==gate_mulinput) {
      mapping[u]=c.setGate(gc.getUUID(u), BooleanGate::MULIN, gc.getProb(u));
      c.setInfo(mapping[u], gc.getInfos(u).first);
      c.addWire(
        mapping[u],
        mapping[gc.getWires(u)[0]]);
    }
  }
  semiring::BoolExpr semiring(c);
  provsql_try_having_boolexpr(gc, semiring, gc.getGate(uuid2string(token)), mapping);
  gate=gc.evaluate(ggate, mapping, semiring);

  return c;
}

GenericCircuit getGenericCircuit(pg_uuid_t token)
{
  return getCircuitFromMMap<GenericCircuit>(token, 'g');
}
