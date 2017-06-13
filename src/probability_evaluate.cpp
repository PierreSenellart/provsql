extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(probability_evaluate);

#define UUID_LEN 16

  /* pg_uuid_t is declared to be struct pg_uuid_t in uuid.h */
  struct pg_uuid_t
  {
    unsigned char data[UUID_LEN];
  };
}

#include "Circuit.h"

using namespace std;

/* copied with small changes from uuid.c */

std::string UUIDDatum2string(Datum token)
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

Datum probability_evaluate(PG_FUNCTION_ARGS)
{
  Datum token = PG_GETARG_DATUM(0);
  Datum token2prob = PG_GETARG_DATUM(1);
  
  if(PG_ARGISNULL(1))
    PG_RETURN_NULL();
  
  constants_t constants;
  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  Datum arguments[2]={token,token2prob};
  Oid argtypes[2]={constants.OID_TYPE_PROVENANCE_TOKEN,REGCLASSOID};
  char nulls[2] = {' ',' '};
  
  SPI_connect();

  Circuit c;

  if(SPI_execute_with_args(
      "SELECT * FROM provsql.sub_circuit_with_prob($1,$2)", 2, argtypes, arguments, nulls, true, 0)
      == SPI_OK_SELECT) {
    int proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    unordered_set<unsigned> already_seen;

    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];

      string f = SPI_getvalue(tuple, tupdesc, 1);
      string type = SPI_getvalue(tuple, tupdesc, 3);
      if(type == "input") {
        c.setGate(f, Circuit::IN, stod(SPI_getvalue(tuple, tupdesc, 4)));
      } else {
        unsigned id=c.getGate(f);

        if(type == "monus") {
          if(already_seen.find(id)!=already_seen.end()) {
            unsigned id2 = c.addGate(Circuit::NOT);
            c.addWire(id, id2);
            elog(WARNING, "Adding wire");
            id=id2;
          } else {
            already_seen.insert(id);
            c.setGate(f, Circuit::AND);
          }
        } else if(type == "plus") {
          c.setGate(f, Circuit::OR);
        } else if(type == "times") {
          c.setGate(f, Circuit::AND);
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

  PG_RETURN_FLOAT8(0.);
}
