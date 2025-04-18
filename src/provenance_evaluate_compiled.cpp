extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "utils/lsyscache.h"
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

static const char *drop_temp_table = "DROP TABLE IF EXISTS tmp_uuids;";

static Datum pec_varchar(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring)
{
  std::unordered_map<gate_t, std::string> provenance_mapping;

  for (uint64 i = 0; i < SPI_processed; i++) {
    HeapTuple tuple = SPI_tuptable->vals[i];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    if (SPI_gettypeid(tupdesc, 2) != constants.OID_TYPE_UUID) {
      SPI_finish();
      throw CircuitException("Invalid type for provenance mapping attribute");
      continue;
    }

    char *value = SPI_getvalue(tuple, tupdesc, 1);
    char *uuid = SPI_getvalue(tuple, tupdesc, 2);

    provenance_mapping[c.getGate(uuid)]=std::string(value);

    pfree(value);
    pfree(uuid);
  }
  SPI_exec(drop_temp_table, 0);
  SPI_finish();

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

static Datum pec_int(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring)
{
  std::unordered_map<gate_t, unsigned> provenance_mapping;

  for (uint64 i = 0; i < SPI_processed; i++) {
    HeapTuple tuple = SPI_tuptable->vals[i];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    if (SPI_gettypeid(tupdesc, 2) != constants.OID_TYPE_UUID) {
      SPI_finish();
      throw CircuitException("Invalid type for provenance mapping attribute");
      continue;
    }

    char *value = SPI_getvalue(tuple, tupdesc, 1);
    char *uuid = SPI_getvalue(tuple, tupdesc, 2);

    provenance_mapping[c.getGate(uuid)]=atoi(value);

    pfree(value);
    pfree(uuid);
  }
  SPI_exec(drop_temp_table, 0);
  SPI_finish();

  if(semiring=="counting") {
    auto val = c.evaluate<semiring::Counting>(g, provenance_mapping);
    PG_RETURN_INT32(val);
  } else
    throw CircuitException("Unknown semiring for type int: "+semiring);
}

void join_with_temp_uuids(Oid table, const std::vector<std::string> &uuids) {
  if (SPI_connect() != SPI_OK_CONNECT)
    throw CircuitException("SPI_connect failed");

  // 1. Create temp table
  const char *create_temp_table = "CREATE TEMP TABLE tmp_uuids(id uuid);";
  if (SPI_exec(create_temp_table, 0) != SPI_OK_UTILITY) {
    SPI_finish();
    throw CircuitException("Failed to create temporary table");
  }

  // 2. Insert UUIDs in batches
  constexpr size_t batch_size = 500;

  for (size_t offset = 0; offset < uuids.size(); offset += batch_size) {
    StringInfoData insert_query;
    initStringInfo(&insert_query);
    appendStringInfo(&insert_query, "INSERT INTO tmp_uuids VALUES ");

    size_t end = std::min(offset + batch_size, uuids.size());
    for (size_t i = offset; i < end; ++i) {
      appendStringInfo(&insert_query, "('%s')%s", uuids[i].c_str(), (i + 1 == end) ? "" : ", ");
    }

    int retval=SPI_exec(insert_query.data, 0);
    pfree(insert_query.data);

    if(retval != SPI_OK_INSERT) {
      SPI_exec(drop_temp_table, 0);
      SPI_finish();
      throw CircuitException("Batch insert into temp table failed");
    }
  }

  // 3. Join with target table
  char *table_name = get_rel_name(table);
  if (!table_name) {
    SPI_exec(drop_temp_table, 0);
    SPI_finish();
    throw CircuitException("Invalid OID: no such table");
  }

  StringInfoData join_query;
  initStringInfo(&join_query);
  appendStringInfo(&join_query,
                   "SELECT value, provenance FROM \"%s\" t JOIN tmp_uuids u ON t.provenance = u.id", table_name);

  if (SPI_exec(join_query.data, 0) != SPI_OK_SELECT) {
    SPI_exec(drop_temp_table, 0);
    SPI_finish();
    throw CircuitException("Join query failed");
  }
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
  join_with_temp_uuids(table, inputs_uuid);

  constants_t constants = get_constants(true);

  if(type==constants.OID_TYPE_VARCHAR)
    return pec_varchar(constants, c, g, inputs, semiring);
  else if(type==constants.OID_TYPE_INT)
    return pec_int(constants, c, g, inputs, semiring);
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
