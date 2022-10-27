extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(probability_evaluate);
}

#include <set>
#include <cmath>
#include <csignal>

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"
#include "dDNNFTreeDecompositionBuilder.h"

using namespace std;

static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;
}

bool operator<(const pg_uuid_t a, const pg_uuid_t b)
{
  return memcmp(&a, &b, sizeof(pg_uuid_t))<0;
}

static Datum probability_evaluate_internal
  (pg_uuid_t token, const string &method, const string &args)
{
  std::set<pg_uuid_t> to_process, processed;
  to_process.insert(token);
  
  BooleanCircuit c;

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
  while(!to_process.empty()) {
    pg_uuid_t uuid = *to_process.begin();
    to_process.erase(to_process.begin());
    processed.insert(uuid);
    std::string f{uuid2string(uuid)};

    bool found;
    provsqlHashEntry *entry = (provsqlHashEntry *) hash_search(provsql_hash, &uuid, HASH_FIND, &found);

    gate_t id;

    if(!found)
      id = c.setGate(f, BooleanGate::MULVAR);
    else {
      switch(entry->type) {
        case gate_input:
          if(isnan(entry->prob)) { 
            LWLockRelease(provsql_shared_state->lock);
            elog(ERROR, "Missing probability for input token");
          }
          id = c.setGate(f, BooleanGate::IN, entry->prob);
          break;

        case gate_mulinput:
          if(isnan(entry->prob)) {
            LWLockRelease(provsql_shared_state->lock);
            elog(ERROR, "Missing probability for input token");
          }
          id = c.setGate(f, BooleanGate::MULIN, entry->prob);
          c.addWire(
              id, 
              c.getGate(uuid2string(provsql_shared_state->wires[entry->children_idx])));
          c.setInfo(id, entry->info1);
          break;

        case gate_times:
        case gate_project:
        case gate_eq:
        case gate_monus:
        case gate_one:
          id = c.setGate(f, BooleanGate::AND);
          break;

        case gate_plus:
        case gate_zero:
          id = c.setGate(f, BooleanGate::OR);
          break;

        default:
            elog(ERROR, "Wrong type of gate in circuit");
        } 

      if(entry->nb_children > 0) {
        if(entry->type == gate_monus) {
          auto id_not = c.setGate(BooleanGate::NOT);
          auto child1 = provsql_shared_state->wires[entry->children_idx];
          auto child2 = provsql_shared_state->wires[entry->children_idx+1];
          c.addWire(
              id,
              c.getGate(uuid2string(child1)));
          c.addWire(id, id_not);
          c.addWire(
              id_not,
              c.getGate(uuid2string(child2)));
          if(processed.find(child1)==processed.end())
            to_process.insert(child1);
          if(processed.find(child2)==processed.end())
            to_process.insert(child2);
        } else {
          for(unsigned i=0;i<entry->nb_children;++i) {
            auto child = provsql_shared_state->wires[entry->children_idx+i];

            c.addWire(
                id, 
                c.getGate(uuid2string(child)));
            if(processed.find(child)==processed.end())
              to_process.insert(child);
          }
        }
      }
    }
  }
  LWLockRelease(provsql_shared_state->lock);

  double result;
  auto gate = c.getGate(uuid2string(token));

  // Display the circuit for debugging:
  // elog(WARNING, "%s", c.toString(gate).c_str());

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  try {
    bool processed=false;

    if(method=="independent") {
      result = c.independentEvaluation(gate);
      processed = true;
    } else if(method=="") {
      // Default evaluation, use independent, tree-decomposition, and
      // compilation in order until one works
      try {
       result = c.independentEvaluation(gate);
       processed = true;
      } catch(CircuitException &) {}
    }

    if(!processed) {
      // Other methods do not deal with multivalued input gates, they
      // need to be rewritten
      c.rewriteMultivaluedGates();

      if(method=="monte-carlo") {
        int samples=0;

        try {
          samples = stoi(args);
        } catch(std::invalid_argument &e) {
        }

        if(samples<=0)
          elog(ERROR, "Invalid number of samples: '%s'", args.c_str());
        
        result = c.monteCarlo(gate, samples);
      } else if(method=="possible-worlds") {
        if(!args.empty())
          elog(WARNING, "Argument '%s' ignored for method possible-worlds", args.c_str());

        result = c.possibleWorlds(gate);
      } else if(method=="compilation") {
        result = c.compilation(gate, args);
      } else if(method=="weightmc") {
        result = c.WeightMC(gate, args);
      } else if(method=="tree-decomposition") {
        try {
          TreeDecomposition td(c);
          auto dnnf{
            dDNNFTreeDecompositionBuilder{
              c,
              uuid2string(token),
              td}.build()
          };
          result = dnnf.dDNNFEvaluation(dnnf.getGate("root"));
        } catch(TreeDecompositionException &) {
          elog(ERROR, "Treewidth greater than %u", TreeDecomposition::MAX_TREEWIDTH);
        }
      } else if(method=="") {
        try {
          TreeDecomposition td(c);
          auto dnnf{
            dDNNFTreeDecompositionBuilder{
              c,
              uuid2string(token),
              td}.build()
          };
          result = dnnf.dDNNFEvaluation(dnnf.getGate("root"));
        } catch(TreeDecompositionException &) {
          result = c.compilation(gate, "d4");
        }
      } else {
        elog(ERROR, "Wrong method '%s' for probability evaluation", method.c_str());
      }
    }
  } catch(CircuitException &e) {
    elog(ERROR, "%s", e.what());
  }

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);
  
  // Avoid rounding errors that make probability outside of [0,1]
  if(result>1.)
    result=1.;
  else if(result<0.)
    result=0.;

  PG_RETURN_FLOAT8(result);
}

Datum probability_evaluate(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    string method;
    string args;
    
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();

    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    return probability_evaluate_internal(*DatumGetUUIDP(token), method, args);
  } catch(const std::exception &e) {
    elog(ERROR, "probability_evaluate: %s", e.what());
  } catch(...) {
    elog(ERROR, "probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}
