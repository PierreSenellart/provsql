/**
 * @file tseytin_cnf.cpp
 * @brief SQL function @c provsql.tseytin_cnf() – return the DIMACS CNF
 *        produced by the Tseytin transformation of a provenance circuit.
 *
 * This is the same encoding that @c BooleanCircuit::compilation() writes
 * to a temp file before invoking @c d4 / @c c2d / @c minic2d / @c dsharp,
 * but returned as a string so the user can inspect it or feed it to a
 * standalone knowledge-compilation tool.
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(tseytin_cnf);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "provsql_utils_cpp.h"

#include <string>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid), @c weighted (bool, default true).
 * Returns: DIMACS CNF as text; when @c weighted, includes @c w lines.
 */
Datum tseytin_cnf(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));

    bool weighted = true;
    if(!PG_ARGISNULL(1))
      weighted = PG_GETARG_BOOL(1);

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    string cnf = c.TseytinCNF(root, weighted);

    text *result = (text *) palloc(VARHDRSZ + cnf.size());
    SET_VARSIZE(result, VARHDRSZ + cnf.size());
    memcpy((void *) VARDATA(result), cnf.c_str(), cnf.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    provsql_error("%s", e.what());
  } catch(...) {
    provsql_error("Unknown exception");
  }
  PG_RETURN_NULL();
}
