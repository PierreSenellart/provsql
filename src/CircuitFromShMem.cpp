#include <cmath>

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_shmem.h"
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

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    bool found;
    provsqlHashEntry *entry = reinterpret_cast<provsqlHashEntry *>(hash_search(provsql_hash, &uuid, HASH_FIND, &found));

    if(!found)
      result.setGate(f, BooleanGate::MULVAR);
    else {
      gate_t id;

      switch(entry->type) {
      case gate_input:
        if(std::isnan(entry->prob)) {
          entry->prob=1.;
        }
        id = result.setGate(f, BooleanGate::IN, entry->prob);
        break;

      case gate_mulinput:
        if(std::isnan(entry->prob)) {
          LWLockRelease(provsql_shared_state->lock);
          elog(ERROR, "Missing probability for mulinput token");
        }
        id = result.setGate(f, BooleanGate::MULIN, entry->prob);
        result.addWire(
          id,
          result.getGate(uuid2string(provsql_shared_state->wires[entry->children_idx])));
        result.setInfo(id, entry->info1);
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

      if(entry->nb_children > 0) {
        if(entry->type == gate_monus) {
          auto id_not = result.setGate(BooleanGate::NOT);
          auto child1 = provsql_shared_state->wires[entry->children_idx];
          auto child2 = provsql_shared_state->wires[entry->children_idx+1];
          result.addWire(
            id,
            result.getGate(uuid2string(child1)));
          result.addWire(id, id_not);
          result.addWire(
            id_not,
            result.getGate(uuid2string(child2)));
          if(processed.find(child1)==processed.end())
            to_process.insert(child1);
          if(processed.find(child2)==processed.end())
            to_process.insert(child2);
        } else {
          for(unsigned i=0; i<entry->nb_children; ++i) {
            auto child = provsql_shared_state->wires[entry->children_idx+i];

            result.addWire(
              id,
              result.getGate(uuid2string(child)));
            if(processed.find(child)==processed.end())
              to_process.insert(child);
          }
        }
      }
    }
  }
  LWLockRelease(provsql_shared_state->lock);

  return result;
}
