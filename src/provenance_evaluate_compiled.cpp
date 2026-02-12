extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "utils/lsyscache.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance_evaluate_compiled);
}

#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include "having_semantics.hpp"
#include "provenance_evaluate_compiled.hpp"
#include "semiring/Boolean.h"
#include "semiring/Counting.h"
#include "semiring/Formula.h"
#include "semiring/Why.h"


const char *drop_temp_table = "DROP TABLE IF EXISTS tmp_uuids;";

static Datum pec_bool(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
  bool drop_table)
{
  std::unordered_map<gate_t, bool> provenance_mapping;
  initialize_provenance_mapping<bool>(constants, c, provenance_mapping, [](const char *v) {
    return *v=='t';
  }, drop_table);

  if(semiring!="boolean")
    throw CircuitException("Unknown semiring for type varchar: "+semiring);

  bool out = false;
  if (!provsql_try_having_boolean(c,g,provenance_mapping,out))
  {
    out = c.evaluate<semiring::Boolean>(g, provenance_mapping);
  } 

  PG_RETURN_BOOL(out);
}

  static Datum pec_why(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
  bool drop_table)
{
  std::unordered_map<gate_t, semiring::why_provenance_t> provenance_mapping;

  initialize_provenance_mapping<semiring::why_provenance_t>(
    constants,
    c,
    provenance_mapping,
    [](const char *v) {
    semiring::why_provenance_t result;
    semiring::label_set single;
    if(strchr(v, '{'))
      elog(ERROR, "Complex Why-semiring values for input tuples not currently supported.");
    single.insert(std::string(v));
    result.insert(std::move(single));
    return result;
  },
    drop_table
    );

  if (semiring != "why")
      throw CircuitException("Unknown semiring for type varchar: " + semiring);

  semiring::why_provenance_t prov;
  if (!provsql_try_having_why(c, g, provenance_mapping, prov)) {
    prov = c.evaluate<semiring::Why>(g, provenance_mapping);
  }

  // Serialize nested set structure: {{x},{y}}
  std::ostringstream oss;
  oss << "{";
  bool firstOuter = true;
  for (const auto &inner : prov) {
    if (!firstOuter) oss << ",";
    firstOuter = false;
    oss << "{";
    bool firstInner = true;
    for (const auto &label : inner) {
      if (!firstInner) oss << ",";
      firstInner = false;
      oss << label;
    }
    oss << "}";
  }
  oss << "}";

  std::string out = oss.str();
  text *result = (text *) palloc(VARHDRSZ + out.size());
  SET_VARSIZE(result, VARHDRSZ + out.size());
  memcpy(VARDATA(result), out.c_str(), out.size());
  PG_RETURN_TEXT_P(result);

}
static Datum pec_varchar(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
  bool drop_table)
{
  std::unordered_map<gate_t, std::string> provenance_mapping;
  initialize_provenance_mapping<std::string>(
    constants, c, provenance_mapping,
    [](const char *v) { return std::string(v); },
    drop_table
  );

  if (semiring!= "formula")
    throw CircuitException("Unknown seimring for type varchar: " + semiring);

  std::string s;
   
  if (!provsql_try_having_formula(c, g, provenance_mapping, s)) {
    s = c.evaluate<semiring::Formula>(g, provenance_mapping);
  }

  text *result = (text *) palloc(VARHDRSZ + s.size());
  SET_VARSIZE(result, VARHDRSZ + s.size());
  memcpy(VARDATA(result), s.c_str(), s.size());
  PG_RETURN_TEXT_P(result);
  
}
static Datum pec_int(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
  bool drop_table)
{
  std::unordered_map<gate_t, unsigned> provenance_mapping;
  initialize_provenance_mapping<unsigned>(constants, c, provenance_mapping, [](const char *v) {
    return atoi(v);
  }, drop_table);

  if(semiring!="counting") 
      throw CircuitException("Unknown semiring for type int: "+semiring);
   unsigned out = 0;
  if (!provsql_try_having_counting(c, g, provenance_mapping, out)) {
    out = c.evaluate<semiring::Counting>(g, provenance_mapping);
  }

  PG_RETURN_INT32((int32) out);

}

bool join_with_temp_uuids(Oid table, const std::vector<std::string> &uuids) {
  if (SPI_connect() != SPI_OK_CONNECT)
    throw CircuitException("SPI_connect failed");

  char *table_name = get_rel_name(table);
  if (!table_name) {
    SPI_finish();
    throw CircuitException("Invalid OID: no such table");
  }

  constexpr size_t nb_max_uuid_value = 10000000;

  StringInfoData join_query;
  initStringInfo(&join_query);
  bool drop_table = false;

  // Two different mechanisms to implement the join (unless there are no
  // UUIDs):
  // if there are less than nb_max_uuid_value UUIDs, we do a join
  // with a VALUES() list;
  // otherwise we create a temporary table where we dump the inserts
  // and join with it.
  if(uuids.size() == 0) {
    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM \"%s\" WHERE 'f'", table_name);
  } else if(uuids.size() <= nb_max_uuid_value) {
    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM \"%s\" t JOIN (VALUES", table_name);
    bool first=true;
    for(auto u: uuids) {
      appendStringInfo(&join_query, "%s('%s'::uuid)", first?"":",", u.c_str());
      first=false;
    }

    appendStringInfo(&join_query, ") AS u(id) ON t.provenance=u.id");
  } else {
    const char *create_temp_table = "CREATE TEMP TABLE tmp_uuids(id uuid);";
    if (SPI_exec(create_temp_table, 0) != SPI_OK_UTILITY) {
      SPI_finish();
      throw CircuitException("Failed to create temporary table");
    }
    drop_table = true;

    constexpr size_t batch_size = 1000;

    for (size_t offset = 0; offset < uuids.size(); offset += batch_size) {
      StringInfoData insert_query;
      initStringInfo(&insert_query);
      appendStringInfo(&insert_query, "INSERT INTO tmp_uuids VALUES ");

      size_t end = std::min(offset + batch_size, uuids.size());
      for (size_t i = offset; i < end; ++i) {
        appendStringInfo(&insert_query, "('%s')%s", uuids[i].c_str(), (i + 1 == end) ? "" : ",");
      }

      int retval=SPI_exec(insert_query.data, 0);
      pfree(insert_query.data);

      if(retval != SPI_OK_INSERT) {
        SPI_exec(drop_temp_table, 0);
        SPI_finish();
        throw CircuitException("Batch insert into temp table failed");
      }
    }

    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM \"%s\" t JOIN tmp_uuids u ON t.provenance = u.id", table_name);
  }

  if (SPI_exec(join_query.data, 0) != SPI_OK_SELECT) {
    if(drop_table)
      SPI_exec(drop_temp_table, 0);
    SPI_finish();
    throw CircuitException("Join query failed");
  }

  return drop_table;
}

static Datum provenance_evaluate_compiled_internal
  (pg_uuid_t token, Oid table, const std::string &semiring, Oid type)
{
  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  auto inputs = c.getInputs();
  std::vector<std::string> inputs_uuid;
  std::transform(inputs.begin(), inputs.end(), std::back_inserter(inputs_uuid), [&c](auto x) {
    return c.getUUID(x);
  });
  bool drop_table = join_with_temp_uuids(table, inputs_uuid);

  constants_t constants = get_constants(true);

  if (type == constants.OID_TYPE_VARCHAR)
  {
    if (semiring == "why")
      return pec_why(constants, c, g, inputs, semiring, drop_table);
    else
      return pec_varchar(constants, c, g, inputs, semiring, drop_table);
  }
  else if(type==constants.OID_TYPE_INT)
    return pec_int(constants, c, g, inputs, semiring, drop_table);
  else if(type==constants.OID_TYPE_BOOL)
    return pec_bool(constants, c, g, inputs, semiring, drop_table);
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
