/**
 * @file compile_to_ddnnf_dot.cpp
 * @brief SQL function @c provsql.compile_to_ddnnf_dot() – render the
 *        compiled d-DNNF of a provenance circuit as GraphViz DOT.
 *
 * Builds the Boolean circuit from the mmap-backed store, runs the
 * requested external knowledge compiler (@c d4 / @c c2d / @c minic2d /
 * @c dsharp) via @c BooleanCircuit::compilation(), and emits the
 * resulting @c dDNNF as a DOT digraph. The string can be piped through
 * @c dot(1) to produce an image, or rendered by Studio.
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

PG_FUNCTION_INFO_V1(compile_to_ddnnf_dot);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "dDNNF.h"
#include "provsql_utils_cpp.h"

#include <string>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid), @c compiler (text, default @c "d4").
 * Returns: DOT representation of the compiled d-DNNF.
 */
Datum compile_to_ddnnf_dot(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));

    string compiler = "d4";
    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      compiler = string(VARDATA(t), VARSIZE(t)-VARHDRSZ);
    }

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    dDNNF d = c.compilation(root, compiler);
    string dot = d.toDot();

    text *result = (text *) palloc(VARHDRSZ + dot.size());
    SET_VARSIZE(result, VARHDRSZ + dot.size());
    memcpy((void *) VARDATA(result), dot.c_str(), dot.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    provsql_error("compile_to_ddnnf_dot: %s", e.what());
  } catch(...) {
    provsql_error("compile_to_ddnnf_dot: Unknown exception");
  }
  PG_RETURN_NULL();
}
