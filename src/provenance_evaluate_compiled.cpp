extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance_evaluate_compiled);
}

#include <string>

#include "provsql_utils_cpp.h"

#include "CircuitFromMMap.h"
#include "Circuit.hpp"
#include "GenericCircuit.hpp"

#include "semiring/Counting.h"

static Datum provenance_evaluate_compiled_internal
  (pg_uuid_t token, Oid table, const std::string &semiring, Oid type)
{
  GenericCircuit c = getGenericCircuit(token);

  if(semiring=="formula") {
    PG_RETURN_INT32(42);
  } else if(semiring=="counting") {
    std::unordered_map<gate_t, unsigned> provenance_mapping;
    auto val = c.evaluate<semiring::Counting>(c.getGate(uuid2string(token)), provenance_mapping);
    PG_RETURN_INT32(val);
  } else {
    throw CircuitException("Unknown semiring: "+semiring);
  }
}

Datum provenance_evaluate_compiled(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);

    Oid table = PG_GETARG_OID(1);

    text *t = PG_GETARG_TEXT_P(2);
    std::string semiring(VARDATA(t),VARSIZE(t)-VARHDRSZ);

    Oid type = get_fn_expr_argtype(fcinfo->flinfo, 3);

    return provenance_evaluate_compiled_internal(*DatumGetUUIDP(token), table, semiring, type);
  } catch(const std::exception &e) {
    elog(ERROR, "provenance_evaluate_compiled: %s", e.what());
  } catch(...) {
    elog(ERROR, "provenance_evaluate_compiled: Unknown exception");
  }

  PG_RETURN_NULL();
}
