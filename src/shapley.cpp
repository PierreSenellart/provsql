extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(shapley);
PG_FUNCTION_INFO_V1(shapley_all_vars);
}

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "CircuitFromShMem.h"
#include <fstream>

using namespace std;

static double shapley_internal
  (pg_uuid_t token, pg_uuid_t variable, const std::string &method, const std::string &args, bool banzhaf)
{
  BooleanCircuit c = createBooleanCircuit(token);

  if(c.getGateType(c.getGate(uuid2string(variable))) != BooleanGate::IN)
    return 0.;

  dDNNF dd = c.makeDD(c.getGate(uuid2string(token)), method, args);

  dd.makeSmooth();
  if(!banzhaf)
    dd.makeGatesBinary(BooleanGate::AND);

  auto var_gate=dd.getGate(uuid2string(variable));

  double result;

  if(!banzhaf)
    result = dd.shapley(var_gate);
  else
    result = dd.banzhaf(var_gate);

  return result;
}

Datum shapley(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
      PG_RETURN_NULL();

    Datum token = PG_GETARG_DATUM(0);
    Datum variable = PG_GETARG_DATUM(1);

    std::string method;
    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    std::string args;
    if(!PG_ARGISNULL(3)) {
      text *t = PG_GETARG_TEXT_P(3);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    bool banzhaf = false;
    if(!PG_ARGISNULL(4)) {
      banzhaf = PG_GETARG_BOOL(4);
    }

    PG_RETURN_FLOAT8(shapley_internal(*DatumGetUUIDP(token), *DatumGetUUIDP(variable), method, args, banzhaf));
  } catch(const std::exception &e) {
    elog(ERROR, "shapley: %s", e.what());
  } catch(...) {
    elog(ERROR, "shapley: Unknown exception");
  }

  PG_RETURN_NULL();
}

Datum shapley_all_vars(PG_FUNCTION_ARGS)
{
  ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

  MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
  MemoryContext oldcontext    = MemoryContextSwitchTo(per_query_ctx);

  TupleDesc tupdesc = rsinfo->expectedDesc;
  Tuplestorestate *tupstore     = tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem);

  rsinfo->returnMode = SFRM_Materialize;
  rsinfo->setResult = tupstore;

  if(!PG_ARGISNULL(0)) {
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));

    std::string method;
    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      method = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    std::string args;
    if(!PG_ARGISNULL(2)) {
      text *t = PG_GETARG_TEXT_P(2);
      args = string(VARDATA(t),VARSIZE(t)-VARHDRSZ);
    }

    bool banzhaf = false;
    if(!PG_ARGISNULL(3)) {
      banzhaf = PG_GETARG_BOOL(3);
    }

    BooleanCircuit c = createBooleanCircuit(token);

    dDNNF dd = c.makeDD(c.getGate(uuid2string(token)), method, args);
    dd.makeSmooth();
    if(!banzhaf)
      dd.makeGatesBinary(BooleanGate::AND);

    for(auto &v_circuit_gate: c.getInputs()) {
      auto var_uuid_string = c.getUUID(v_circuit_gate);
      auto var_gate=dd.getGate(var_uuid_string);
      pg_uuid_t *uuidp = reinterpret_cast<pg_uuid_t*>(palloc(UUID_LEN));
      *uuidp = string2uuid(var_uuid_string);

      double result;

      if(!banzhaf)
        result = dd.shapley(var_gate);
      else
        result = dd.banzhaf(var_gate);

      Datum values[2] = {
        UUIDPGetDatum(uuidp), Float8GetDatum(result)
      };
      bool nulls[sizeof(values)] = {0, 0};

      tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
  }

  tuplestore_donestoring(tupstore);
  MemoryContextSwitchTo(oldcontext);

  PG_RETURN_NULL();
}
