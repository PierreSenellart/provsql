extern "C" {
#include "postgres.h"
#include "fmgr.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
#include "catalog/pg_type.h"
#include "utils/jsonb.h"
#include "utils/fmgrprotos.h"
#include "utils/uuid.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(circuit_canonical_form);
PG_FUNCTION_INFO_V1(circuit_island_key);
PG_FUNCTION_INFO_V1(canonical_cache_stats);
PG_FUNCTION_INFO_V1(canonical_cache_reset);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CanonicalCache.h"
#include "CircuitCanonicalForm.h"
#include "CircuitFromMMap.h"
#include "provsql_utils_cpp.h"

#include <sstream>
#include <string>

using namespace std;

namespace {

BooleanCircuit booleanViewOf(pg_uuid_t token, gate_t &root)
{
  BooleanCircuit c = getBooleanCircuit(token, root);
  c.rewriteMultivaluedGates();
  return c;
}

}

Datum circuit_canonical_form(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));

    gate_t root;
    BooleanCircuit c = booleanViewOf(token, root);
    provsql::CanonicalForm f = provsql::computeCanonicalForm(c, root);

    text *result = reinterpret_cast<text*>(
      palloc(f.form.size() + VARHDRSZ));
    SET_VARSIZE(result, f.form.size() + VARHDRSZ);
    memcpy(VARDATA(result), f.form.data(), f.form.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    provsql_error("circuit_canonical_form: %s", e.what());
  } catch(...) {
    provsql_error("circuit_canonical_form: Unknown exception");
  }
  PG_RETURN_NULL();
}

Datum circuit_island_key(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t token = *DatumGetUUIDP(PG_GETARG_DATUM(0));

    gate_t root;
    BooleanCircuit c = booleanViewOf(token, root);
    provsql::IslandKey key = provsql::computeIslandKey(c, root);

    std::ostringstream out;
    out << "residual: " << key.residual.form;
    for(size_t i = 0; i < key.islands.size(); ++i)
      out << "\nisland " << (i + 1) << ": " << key.islands[i].form;
    const std::string s = out.str();

    text *result = reinterpret_cast<text*>(palloc(s.size() + VARHDRSZ));
    SET_VARSIZE(result, s.size() + VARHDRSZ);
    memcpy(VARDATA(result), s.data(), s.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    provsql_error("circuit_island_key: %s", e.what());
  } catch(...) {
    provsql_error("circuit_island_key: Unknown exception");
  }
  PG_RETURN_NULL();
}

Datum canonical_cache_stats(PG_FUNCTION_ARGS)
{
  try {
    provsql::CanonicalCacheStats s =
      provsql::CanonicalCache::instance().stats();
    std::ostringstream out;
    out << '{'
        << "\"hits\":" << s.hits
        << ",\"misses\":" << s.misses
        << ",\"inserts\":" << s.inserts
        << ",\"evictions\":" << s.evictions
        << ",\"rejections\":" << s.rejections
        << ",\"entries\":" << s.entries
        << ",\"bytes\":" << s.bytes
        << '}';
    Datum json_datum = DirectFunctionCall1(
      jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
    PG_RETURN_DATUM(json_datum);
  } catch(const std::exception &e) {
    provsql_error("canonical_cache_stats: %s", e.what());
  } catch(...) {
    provsql_error("canonical_cache_stats: Unknown exception");
  }
  PG_RETURN_NULL();
}

Datum canonical_cache_reset(PG_FUNCTION_ARGS)
{
  provsql::CanonicalCache::instance().reset();
  PG_RETURN_VOID();
}
