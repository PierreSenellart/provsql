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
#include "semiring/Formula.h"

static Datum pec_varchar(const GenericCircuit &c, gate_t g, const std::set<gate_t> inputs, const std::string semiring) {
  std::unordered_map<gate_t, std::string> provenance_mapping;

  /* TODO: Populate a provenance mapping (of the right type!) querying
   * the table for the mapping of every input */

  if(semiring=="formula") {
    auto s = c.evaluate<semiring::Formula>(g, provenance_mapping);
    text *result = (text *) palloc(VARHDRSZ + s.size() + 1);
    SET_VARSIZE(result, VARHDRSZ + s.size());

    memcpy((void *) VARDATA(result),
           s.c_str(),
           s.size());
    PG_RETURN_TEXT_P(result);
  } else
    throw CircuitException("Unknown semiring for type varchar: "+semiring);
}

static Datum pec_int(const GenericCircuit &c, gate_t g, const std::set<gate_t> inputs, const std::string semiring) {
  std::unordered_map<gate_t, unsigned> provenance_mapping;

  /* TODO: Populate a provenance mapping (of the right type!) querying
   * the table for the mapping of every input */

  if(semiring=="counting") {
    auto val = c.evaluate<semiring::Counting>(g, provenance_mapping);
    PG_RETURN_INT32(val);
  } else
    throw CircuitException("Unknown semiring for type int: "+semiring);
}

static Datum provenance_evaluate_compiled_internal
  (pg_uuid_t token, Oid table, const std::string &semiring, Oid type)
{
  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  auto inputs = c.getInputs();

  constants_t constants = get_constants(true);

  if(type==constants.OID_TYPE_VARCHAR)
    return pec_varchar(c, g, inputs, semiring);
  else if(type==constants.OID_TYPE_INT)
    return pec_int(c, g, inputs, semiring);
  else
    throw CircuitException("Unknown element type for provenance_evaluate_compiled");
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
