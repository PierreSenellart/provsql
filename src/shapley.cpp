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

using namespace std;

static void provsql_sigint_handler (int)
{
  provsql_interrupted = true;
}

static bool operator<(const pg_uuid_t a, const pg_uuid_t b)
{
  return memcmp(&a, &b, sizeof(pg_uuid_t))<0;
}

static Datum shapley_internal
  (pg_uuid_t token, pg_uuid_t variable)
{
  BooleanCircuit c(token);
  auto gate = c.getGate(uuid2string(token));

  provsql_interrupted = false;
  void (*prev_sigint_handler)(int);
  prev_sigint_handler = signal(SIGINT, provsql_sigint_handler);

  double result=0.;

  try {
    TreeDecomposition td(c);
    auto dnnf{
      dDNNFTreeDecompositionBuilder{
        c, uuid2string(token), td}.build()
    };
    result = dnnf.shapley(dnnf.getGate("root"),"token");
  } catch(TreeDecompositionException &) {
    provsql_interrupted = false;
    signal (SIGINT, prev_sigint_handler);
    elog(ERROR, "Treewidth greater than %u", TreeDecomposition::MAX_TREEWIDTH);
  }

  provsql_interrupted = false;
  signal (SIGINT, prev_sigint_handler);

  // Avoid rounding errors that make expected Shapley value outside of [-1,1]
  if(result>1.)
    result=1.;
  else if(result<-1.)
    result=-1.;

  PG_RETURN_FLOAT8(result);
}

Datum shapley(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);
    Datum variable = PG_GETARG_DATUM(1)

                     if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
      PG_RETURN_NULL();

    return shapley_internal(*DatumGetUUIDP(token), *DatumGetUUIDP(variable));
  } catch(const std::exception &e) {
    elog(ERROR, "shapley: %s", e.what());
  } catch(...) {
    elog(ERROR, "shapley: Unknown exception");
  }

  PG_RETURN_NULL();
}
