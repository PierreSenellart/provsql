extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(view_circuit);

}

#include "DotCircuit.h"
#include <csignal>

using namespace std;

/* copied with small changes from uuid.c */

static std::string UUIDDatum2string(Datum token)
{
  pg_uuid_t *uuid = DatumGetUUIDP(token);
  static const char hex_chars[] = "0123456789abcdef";
  string result;

  for (int i = 0; i < UUID_LEN; i++)
  {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      result += '-';

    int hi = uuid->data[i] >> 4;
    int lo = uuid->data[i] & 0x0F;

    result+=hex_chars[hi];
    result+=hex_chars[lo];
  }

  return result;
}

static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;
}

static Datum view_circuit_internal(Datum token, Datum token2prob)
{
  constants_t constants;
  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  Datum arguments[2]={token,token2prob};
  Oid argtypes[2]={constants.OID_TYPE_PROVENANCE_TOKEN,REGCLASSOID};
  char nulls[2] = {' ',' '};
  
  SPI_connect();

  DotCircuit c;

  if(SPI_execute_with_args(
      "SELECT * FROM provsql.sub_circuit_with_desc($1,$2)", 2, argtypes, arguments, nulls, true, 0)
      == SPI_OK_SELECT) {
    int proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    elog(WARNING, "Query executed successfully");
    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];

      string f = SPI_getvalue(tuple, tupdesc, 1);
      string type = SPI_getvalue(tuple, tupdesc, 3);
      if(type == "input") {
        elog(WARNING, "Found an input gate");
        c.setGate(f, DotGate::IN, SPI_getvalue(tuple, tupdesc, 4));
      } else {
        unsigned id=c.getGate(f);

        if(type == "monus" || type == "monusl" || type == "times" || type=="project") {
          c.setGate(f, DotGate::OTIMES);
        } else if(type == "plus") {
          c.setGate(f, DotGate::OPLUS);
        } else if(type == "monusr") {
          c.setGate(f, DotGate::OMINUS);
        } else {
          elog(ERROR, "Wrong type of gate in circuit");
        }
        c.addWire(id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
      }
    }
  }

  SPI_finish();

  // Display the circuit for debugging:
  elog(WARNING, "Dot:\n %s", c.toString(0).c_str());

  //Calling the dot renderer
  int result = c.render();

  provsql_interrupted = false;

  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);
  
  PG_RETURN_FLOAT8(result);
}

Datum view_circuit(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    Datum token2prob = PG_GETARG_DATUM(1);

    if(PG_ARGISNULL(1))
      PG_RETURN_NULL();

    return view_circuit_internal(token, token2prob);
  } catch(const std::exception &e) {
    elog(ERROR, "view_circuit: %s", e.what());
  } catch(...) {
    elog(ERROR, "view_circuit: Unknown exception");
  }

  PG_RETURN_NULL();
}
