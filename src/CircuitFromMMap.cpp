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
#include "RangeCheck.h"
#include "having_semantics.hpp"
#include "semiring/BoolExpr.h"
#include "provsql_utils_cpp.h"

#include <vector>

extern "C" {
#include "miscadmin.h"
#include "provsql_shmem.h"
#include "provsql_mmap.h"
#include "provsql_utils.h"
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

/**
 * @brief Apply the universal load-time simplification passes to @p gc.
 *
 * Extracted from @c getGenericCircuit so the joint-circuit loader
 * (@c getJointCircuit) runs the same passes on its multi-root output.
 * Gated by the @c provsql.simplify_on_load GUC, identical semantics.
 */
static void applyLoadTimeSimplification(GenericCircuit &gc)
{
  if (provsql_simplify_on_load) {
    provsql::runRangeCheck(gc);
    gc.foldSemiringIdentities();
  }
}

GenericCircuit getGenericCircuit(pg_uuid_t token)
{
  GenericCircuit gc = getCircuitFromMMap<GenericCircuit>(token, 'g');

  /* Apply universal cmp-resolution passes (currently RangeCheck) at
   * load time so every downstream consumer -- semiring evaluators,
   * MC, view_circuit, PROV export -- sees the simplified circuit.
   * Each pass replaces decided gate_cmps with Bernoulli gate_input
   * gates with probability 0 or 1 via
   * GenericCircuit::resolveCmpToBernoulli; the rewrite is uniform
   * across semirings (gate_zero / gate_one are the additive /
   * multiplicative identities in any semiring) so a single sweep
   * here is correct for every later evaluator.
   *
   * Gated by the provsql.simplify_on_load GUC so users debugging
   * raw circuit structure can opt out. */
  applyLoadTimeSimplification(gc);

  return gc;
}

/**
 * @brief IPC: ship a 'j' (joint) request to the mmap worker and
 *        deserialise the returned @c GenericCircuit.
 *
 * Mirrors @c getCircuitFromMMap but writes the multi-root payload
 * the worker's @c 'j' handler expects: <tt>'j' Oid nb_roots {pg_uuid_t}*</tt>.
 * The response shape is identical to @c 'g' -- @c unsigned @c long
 * size prefix followed by a Boost-serialised @c GenericCircuit.
 */
static GenericCircuit getJointCircuitFromMMap(
    pg_uuid_t root_token, pg_uuid_t event_token)
{
  char message_char = 'j';
  unsigned nb_roots = 2;
  STARTWRITEM();
  ADDWRITEM(&message_char, char);
  ADDWRITEM(&MyDatabaseId, Oid);
  ADDWRITEM(&nb_roots, unsigned);
  ADDWRITEM(&root_token, pg_uuid_t);
  ADDWRITEM(&event_token, pg_uuid_t);

  provsql_shmem_lock_exclusive();
  if(!SENDWRITEM())
    provsql_error("Cannot write to pipe (message type j)");

  unsigned long size;
  if(!READB(size, unsigned long))
    provsql_error("Cannot read from pipe (message type j)");

  char *buf = new char[size], *p = buf;
  ssize_t actual_read, remaining_size=size;
  while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
    if(actual_read<=0) {
      provsql_shmem_unlock();
      delete [] buf;
      provsql_error("Cannot read from pipe (message type j)");
    } else {
      remaining_size-=actual_read;
      p+=actual_read;
    }
  }
  provsql_shmem_unlock();

  boost::iostreams::stream<boost::iostreams::array_source> stream(buf, size);
  boost::archive::binary_iarchive ia(stream);
  GenericCircuit c;
  ia >> c;

  delete [] buf;

  return c;
}

GenericCircuit getJointCircuit(
  pg_uuid_t root_token,
  pg_uuid_t event_token,
  gate_t &root_gate,
  gate_t &event_gate)
{
  GenericCircuit gc = getJointCircuitFromMMap(root_token, event_token);

  applyLoadTimeSimplification(gc);

  /* Resolve the gate_t for the two roots AFTER simplification.  The
   * passes mutate gate types in place but never delete gates, so the
   * UUID-to-gate_t map (and therefore @c getGate) stays valid. */
  root_gate  = gc.getGate(uuid2string(root_token));
  event_gate = gc.getGate(uuid2string(event_token));

  return gc;
}
