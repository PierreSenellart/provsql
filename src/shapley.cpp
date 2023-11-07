extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(shapley);
}

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"
#include "dDNNFTreeDecompositionBuilder.h"
#include "CircuitFromShMem.h"
#include <fstream>

using namespace std;

static double shapley_internal
  (pg_uuid_t token, pg_uuid_t variable)
{
  BooleanCircuit c = createBooleanCircuit(token);

  if(c.getGateType(c.getGate(uuid2string(variable))) != BooleanGate::IN)
    return 0.;

  dDNNF dd = c.makeDD(c.getGate(uuid2string(token)));

  dd.makeSmooth();
  dd.makeGatesBinary(BooleanGate::AND);

  auto var_gate=dd.getGate(uuid2string(variable));

/*
   std::string filename("/tmp/export.dd");
   std::ofstream o(filename.c_str());
   o << dd.exportCircuit(root);
   o.close();
 */

  double result = dd.shapley(var_gate);

  // Avoid rounding errors that make expected Shapley value outside of [-1,1]
  if(result>1.)
    result=1.;
  else if(result<-1.)
    result=-1.;

  return result;
}

Datum shapley(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    Datum variable = PG_GETARG_DATUM(1);

    if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
      PG_RETURN_NULL();

    PG_RETURN_FLOAT8(shapley_internal(*DatumGetUUIDP(token), *DatumGetUUIDP(variable)));
  } catch(const std::exception &e) {
    elog(ERROR, "shapley: %s", e.what());
  } catch(...) {
    elog(ERROR, "shapley: Unknown exception");
  }

  PG_RETURN_NULL();
}
