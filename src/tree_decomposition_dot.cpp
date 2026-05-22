/**
 * @file tree_decomposition_dot.cpp
 * @brief SQL function @c provsql.tree_decomposition_dot() – return a
 *        GraphViz DOT visualisation of the tree decomposition of the
 *        provenance circuit's primal graph.
 *
 * The same min-fill decomposition that backs
 * @c probability_evaluate(..., 'tree-decomposition') is exposed here as
 * a DOT digraph, with the computed treewidth attached as a
 * @c "// treewidth=..." comment on the first line.
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

PG_FUNCTION_INFO_V1(tree_decomposition_dot);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "TreeDecomposition.h"
#include "provsql_utils_cpp.h"

#include <string>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid).
 * Returns: DOT text whose first line is @c "// treewidth=<n>".
 */
Datum tree_decomposition_dot(PG_FUNCTION_ARGS)
{
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    try {
      TreeDecomposition td(c);
      string dot = "// treewidth=" + to_string(td.getTreewidth()) + "\n"
                   + td.toDot();
      text *result = (text *) palloc(VARHDRSZ + dot.size());
      SET_VARSIZE(result, VARHDRSZ + dot.size());
      memcpy((void *) VARDATA(result), dot.c_str(), dot.size());
      PG_RETURN_TEXT_P(result);
    } catch(TreeDecompositionException &) {
      provsql_error(
        "tree_decomposition_dot: circuit treewidth exceeds the supported "
        "limit (MAX_TREEWIDTH)");
    }
  } catch(const std::exception &e) {
    provsql_error("tree_decomposition_dot: %s", e.what());
  } catch(...) {
    provsql_error("tree_decomposition_dot: Unknown exception");
  }
  PG_RETURN_NULL();
}
