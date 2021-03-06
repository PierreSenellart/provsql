extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(probability_evaluate);
}

#include <csignal>

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"
#include "dDNNFTreeDecompositionBuilder.h"

using namespace std;

static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;
}

static Datum probability_evaluate_internal
  (Datum token, Datum token2prob, const string &method, const string &args)
{
  constants_t constants;
  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  Datum arguments[2]={token,token2prob};
  Oid argtypes[2]={constants.OID_TYPE_PROVENANCE_TOKEN,REGCLASSOID};
  char nulls[2] = {' ',' '};
  
  SPI_connect();

  BooleanCircuit c;

  if(SPI_execute_with_args(
      "SELECT * FROM provsql.sub_circuit_with_prob($1,$2)", 2, argtypes, arguments, nulls, true, 0)
      == SPI_OK_SELECT) {
    int proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];

      string f = SPI_getvalue(tuple, tupdesc, 1);
      string type = SPI_getvalue(tuple, tupdesc, 3);
      if(type == "input") {
        c.setGate(f, BooleanGate::IN, stod(SPI_getvalue(tuple, tupdesc, 4)));
      } else {
        auto id=c.getGate(f);

        if(type == "monus" || type == "monusl" || type == "times" || type=="project" || type=="eq") {
          c.setGate(f, BooleanGate::AND);
        } else if(type == "plus") {
          c.setGate(f, BooleanGate::OR);
        } else if(type == "monusr") {
          c.setGate(f, BooleanGate::NOT);
        } else {
          elog(ERROR, "Wrong type of gate in circuit");
        }
        c.addWire(id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
      }
    }
  }

  SPI_finish();

// Display the circuit for debugging:
// elog(WARNING, "%s", c.toString(c.getGate(UUIDDatum2string(token))).c_str());

  double result;
  auto gate = c.getGate(UUIDDatum2string(token));

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  try {
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
            UUIDDatum2string(token),
            td}.build()
        };
        result = dnnf.dDNNFEvaluation(dnnf.getGate("root"));
      } catch(TreeDecompositionException &) {
        elog(ERROR, "Treewidth greater than %u", TreeDecomposition::MAX_TREEWIDTH);
      }
    } else if(method=="independent") {
      result = c.independentEvaluation(gate);
    } else if(method=="") {
      // Default evaluation, use independent, tree-decomposition, and
      // compilation in order until one works
      try {
       result = c.independentEvaluation(gate);
      } catch(CircuitException &) {
        try {
          TreeDecomposition td(c);
          auto dnnf{
            dDNNFTreeDecompositionBuilder{
              c,
                UUIDDatum2string(token),
                td}.build()
          };
          result = dnnf.dDNNFEvaluation(dnnf.getGate("root"));
        } catch(TreeDecompositionException &) {
          result = c.compilation(gate, "d4");
        }
      }
    } else {
      elog(ERROR, "Wrong method '%s' for probability evaluation", method.c_str());
    }
  } catch(CircuitException &e) {
    elog(ERROR, "%s", e.what());
  }

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);
  
  PG_RETURN_FLOAT8(result);
}

Datum probability_evaluate(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    Datum token2prob = PG_GETARG_DATUM(1);
    string method;
    string args;

    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    if(!PG_ARGISNULL(3)) {
      text *t = PG_GETARG_TEXT_P(3);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    if(PG_ARGISNULL(1))
      PG_RETURN_NULL();

    return probability_evaluate_internal(token, token2prob, method, args);
  } catch(const std::exception &e) {
    elog(ERROR, "probability_evaluate: %s", e.what());
  } catch(...) {
    elog(ERROR, "probability_evaluate: Unknown exception");
  }

  PG_RETURN_NULL();
}
