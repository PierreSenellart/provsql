/**
 * @file CircuitFromMMap.cpp
 * @brief Build in-memory circuits from the mmap-backed store.
 *
 * Implements the free functions declared in @c CircuitFromMMap.h:
 * - @c getBooleanCircuit(): reads the mmap store (via the background
 *   worker IPC channel) and constructs a @c BooleanCircuit.
 * - @c getGenericCircuit(): same but constructs a @c GenericCircuit.
 *
 * The internal @c getCircuitFromMMap<C>() template handles the IPC
 * protocol: it sends a request through the shared-memory pipe,
 * receives a Boost-serialised circuit blob from the background worker,
 * and deserialises it into the appropriate circuit type.
 */
#include <cmath>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#include "CircuitFromMMap.h"
#include "having_semantics.hpp"
#include "semiring/BoolExpr.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "miscadmin.h"
#include "provsql_shmem.h"
#include "provsql_mmap.h"
}

/**
 * @brief Read and deserialise a circuit rooted at @p token from the mmap worker.
 * @tparam C           Circuit type to deserialise (@c BooleanCircuit or @c GenericCircuit).
 * @param token        UUID of the root gate to retrieve.
 * @param message_char IPC message-type byte sent to the background worker.
 * @return             Deserialised circuit of type @c C.
 */
template<typename C>
static C getCircuitFromMMap(pg_uuid_t token, char message_char)
{
  STARTWRITEM();
  ADDWRITEM(&message_char, char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(&token, pg_uuid_t);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM())
    provsql_error("Cannot write to pipe (message type %c)", message_char);

  unsigned long size;
  if(!READB(size, unsigned long))
    provsql_error("Cannot read from pipe (message type %c)", message_char);

  char *buf = new char[size], *p = buf;
  ssize_t actual_read, remaining_size=size;
  while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
    if(actual_read<=0) {
      provsql_shmem_unlock();
      delete [] buf;
      provsql_error("Cannot read from pipe (message type %c)", message_char);
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

BooleanCircuit getBooleanCircuit(
  GenericCircuit &gc,
  pg_uuid_t token,
  gate_t &gate,
  std::unordered_map<gate_t, gate_t> &gc_to_bc)
{
  auto ggate = gc.getGate(uuid2string(token));
  BooleanCircuit c;
  for(gate_t u: gc.getInputs()) {
    gc_to_bc[u]=c.setGate(gc.getUUID(u), BooleanGate::IN, gc.getProb(u));
  }
  for(size_t i=0; i<gc.getNbGates(); ++i) {
    auto u=static_cast<gate_t>(i);
    if(gc.getGateType(u)==gate_mulinput) {
      gc_to_bc[u]=c.setGate(gc.getUUID(u), BooleanGate::MULIN, gc.getProb(u));
      c.setInfo(gc_to_bc[u], gc.getInfos(u).first);
      c.addWire(
        gc_to_bc[u],
        gc_to_bc[gc.getWires(u)[0]]);
    }
  }
  semiring::BoolExpr semiring(c);
  provsql_having(gc, ggate, gc_to_bc, semiring);
  gate=gc.evaluate(ggate, gc_to_bc, semiring);

  return c;
}

BooleanCircuit getBooleanCircuit(pg_uuid_t token, gate_t &gate)
{
  GenericCircuit gc = getGenericCircuit(token);
  std::unordered_map<gate_t, gate_t> gc_to_bc;
  return getBooleanCircuit(gc, token, gate, gc_to_bc);
}

GenericCircuit getGenericCircuit(pg_uuid_t token)
{
  return getCircuitFromMMap<GenericCircuit>(token, 'g');
}
