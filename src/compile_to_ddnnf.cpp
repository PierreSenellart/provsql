/**
 * @file compile_to_ddnnf.cpp
 * @brief SQL function @c provsql.compile_to_ddnnf() – return the
 *        compiled d-DNNF of a provenance circuit as c2d / d4 ".nnf" text.
 *
 * Companion to @c compile_to_ddnnf_dot (which returns GraphViz DOT for a
 * human): this returns the standard NNF interchange format, so the
 * compiled circuit can be fed to an external d-DNNF reasoner / verifier
 * or saved alongside the @c tseytin_cnf output (the two share the same
 * variable numbering). Dispatch is the shared @c makeDDByName, so it
 * accepts the same compiler / meta-route names as the DOT version.
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

PG_FUNCTION_INFO_V1(compile_to_ddnnf);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "dDNNF.h"
#include "provsql_utils_cpp.h"
#include "tool_registry_sync.h"

#include <string>
#include <unordered_map>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid), @c compiler (text, default @c "d4").
 * Returns: the compiled d-DNNF in NNF text format.
 */
Datum compile_to_ddnnf(PG_FUNCTION_ARGS)
{
  provsql_sync_tool_registry();  // honour persisted tool-registry overrides
  try {
    if(PG_ARGISNULL(0))
      PG_RETURN_NULL();
    pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));

    string compiler = "";  // empty => preference-ranked best available compiler
    if(!PG_ARGISNULL(1)) {
      text *t = PG_GETARG_TEXT_P(1);
      compiler = string(VARDATA(t), VARSIZE(t)-VARHDRSZ);
    }

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    // Map each input's UUID to its Tseytin-CNF variable (gate id + 1),
    // the same numbering tseytin_cnf / tseytin_cnf_mapping use. Passing
    // this into toNNF makes the NNF's input variables agree with the
    // CNF even when an external compiler renumbered them internally.
    std::unordered_map<std::string, int> uuid2var;
    for(gate_t in : c.getInputs())
      uuid2var[c.getUUID(in)] =
        static_cast<int>(static_cast<std::underlying_type<gate_t>::type>(in)) + 1;
    auto var_of_uuid = [&uuid2var](const std::string &u) -> int {
      auto it = uuid2var.find(u);
      return it == uuid2var.end() ? -1 : it->second;
    };

    // "inversion-free" builds the structured d-DNNF over the order keys on the
    // generic circuit (see buildInversionFreeDDNNF); its input leaves carry the
    // same UUIDs as c, so var_of_uuid still aligns the NNF with the CNF.
    dDNNF d = (compiler == "inversion-free")
      ? buildInversionFreeDDNNF(*token)
      : c.makeDDByName(root, compiler);
    string nnf = d.toNNF(var_of_uuid);

    text *result = (text *) palloc(VARHDRSZ + nnf.size());
    SET_VARSIZE(result, VARHDRSZ + nnf.size());
    memcpy((void *) VARDATA(result), nnf.c_str(), nnf.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    provsql_error("%s", e.what());
  } catch(...) {
    provsql_error("Unknown exception");
  }
  PG_RETURN_NULL();
}
