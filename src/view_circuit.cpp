extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(view_circuit);

#if PG_VERSION_NUM < 100000
/* In versions of PostgreSQL < 10, pg_uuid_t is declared to be an opaque
 * struct pg_uuid_t in uuid.h, so we have to give the definition of
 * struct pg_uuid_t; this problem is resolved in PostgreSQL 10 */
#define UUID_LEN 16
  struct pg_uuid_t
  {
    unsigned char data[UUID_LEN];
  };
#endif /* PG_VERSION_NUM */
}

#include "Circuit.h"
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

static Datum view_circuit_internal
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

  Circuit c;

  //TODO: this part needs to be changed with the proper semantics of the input
  //gates
  //Possible input <Relation.id>
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
        c.setGate(f, Circuit::IN, stod(SPI_getvalue(tuple, tupdesc, 4)));
      } else {
        unsigned id=c.getGate(f);

        if(type == "monus" || type == "monusl" || type == "times" || type=="project") {
          c.setGate(f, Circuit::AND);
        } else if(type == "plus") {
          c.setGate(f, Circuit::OR);
        } else if(type == "monusr") {
          c.setGate(f, Circuit::NOT);
        } else {
          elog(ERROR, "Wrong type of gate in circuit");
        }
        c.addWire(id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
      }
    }
  }

  SPI_finish();

// Display the circuit for debugging:
// TODO add the DOT output
 elog(WARNING, "Formula: %s\n", c.toString(c.getGate(UUIDDatum2string(token))).c_str());
 elog(WARNING, "Dot:\n %s", c.toDot().c_str());

//Calling the dot renderer
  int result = c.dotRenderer();



  unsigned gate = c.getGate(UUIDDatum2string(token));

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

    return view_circuit_internal(token, token2prob, method, args);
  } catch(const std::exception &e) {
    elog(ERROR, "view_circuit: %s", e.what());
  } catch(...) {
    elog(ERROR, "view_circuit: Unknown exception");
  }

  PG_RETURN_NULL();
}
