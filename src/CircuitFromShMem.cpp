#include <cmath>

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_shmem.h"
#include "provsql_mmap.h"
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

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    gate_t id;

    gate_type type;
    double prob;
    int info1, info2;
    unsigned nb_children;
    std::vector<pg_uuid_t> children;

    if(!WRITEM("t", char) || !WRITEM(&uuid, pg_uuid_t)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type t)");
    }
    if(!READB(type, gate_type)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot read response from pipe (message type t)");
    }

    if(!WRITEM("p", char) || !WRITEM(&uuid, pg_uuid_t)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type p)");
    }
    if(!READB(prob, double)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot read response from pipe (message type p)");
    }

    if(!WRITEM("i", char) || !WRITEM(&uuid, pg_uuid_t)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type i)");
    }
    if(!READB(info1, int) || !READB(info2, int)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot read response from pipe (message type i)");
    }

    if(!WRITEM("c", char) || !WRITEM(&uuid, pg_uuid_t)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot write to pipe (message type c)");
    }
    if(!READB(nb_children, unsigned)) {
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Cannot read response from pipe (message type c)");
    }
    for(unsigned i=0; i<nb_children; ++i) {
      pg_uuid_t child;
      if(!READB(child, pg_uuid_t)) {
        LWLockRelease(provsql_shared_state->lock);
        elog(ERROR, "Cannot read response from pipe (message type c)");
      }
      children.push_back(child);
    }

    switch(type) {
    case gate_input:
      if(std::isnan(prob)) {
        prob=1.;
      }
      id = result.setGate(f, BooleanGate::IN, prob);
      break;

    case gate_mulinput:
      if(std::isnan(prob)) {
        LWLockRelease(provsql_shared_state->lock);
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
      LWLockRelease(provsql_shared_state->lock);
      elog(ERROR, "Wrong type of gate in circuit");
    }

    if(nb_children > 0 && type != gate_mulinput) {
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
        for(unsigned i=0; i<nb_children; ++i) {
          result.addWire(
            id,
            result.getGate(uuid2string(children[i])));
          if(processed.find(children[i])==processed.end())
            to_process.insert(children[i]);
        }
      }
    }
  }

  LWLockRelease(provsql_shared_state->lock);

  return result;
}
