/**
 * @file canonical_form_sql.cpp
 * @brief SQL functions @c provsql.circuit_canonical_form(),
 *        @c provsql.circuit_island_key(), @c provsql.prob_cache_stats()
 *        and @c provsql.prob_cache_reset() – inspect the canonical form
 *        of a circuit and the cache of compiled d-DNNFs.
 *
 * Diagnostics for the cache of @c ProbCache.h: the first two show the
 * keys a circuit is looked up under, the last two read and empty the
 * cache of the current session.
 */
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
PG_FUNCTION_INFO_V1(prob_cache_stats);
PG_FUNCTION_INFO_V1(prob_cache_reset);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "ProbCache.h"
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

Datum prob_cache_stats(PG_FUNCTION_ARGS)
{
  try {
    provsql::ProbCacheStats s =
      provsql::ProbCache::instance().stats();
    std::ostringstream out;
    out << '{'
        << "\"hits\":" << s.hits
        << ",\"misses\":" << s.misses
        << ",\"inserts\":" << s.inserts
        << ",\"evictions\":" << s.evictions
        << ",\"rejections\":" << s.rejections
        << ",\"entries\":" << s.entries
        << ",\"bytes\":" << s.bytes
        << ",\"budget\":" << s.budget
        << '}';
    Datum json_datum = DirectFunctionCall1(
      jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
    PG_RETURN_DATUM(json_datum);
  } catch(const std::exception &e) {
    provsql_error("prob_cache_stats: %s", e.what());
  } catch(...) {
    provsql_error("prob_cache_stats: Unknown exception");
  }
  PG_RETURN_NULL();
}

Datum prob_cache_reset(PG_FUNCTION_ARGS)
{
  provsql::ProbCache::instance().reset();
  PG_RETURN_VOID();
}
