/**
 * @file ddnnf_stats.cpp
 * @brief SQL function @c provsql.ddnnf_stats() – structural statistics
 *        of the d-DNNF a given compiler produces for a provenance
 *        circuit.
 *
 * Compiles the Boolean circuit with the requested compiler / meta-route
 * (the same @c makeDDByName dispatch used by @c compile_to_ddnnf_dot),
 * then reports node / edge / gate-type counts, smoothness, longest-path
 * depth, the wall-clock compile time, and the circuit's treewidth (when
 * computable). This makes "compare what each compiler produces on the
 * same circuit" a measurable operation rather than an eyeball of DOT.
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

PG_FUNCTION_INFO_V1(ddnnf_stats);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "dDNNF.h"
#include "TreeDecomposition.h"
#include "provsql_utils_cpp.h"
#include "tool_registry_sync.h"

#include <chrono>
#include <sstream>
#include <string>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid), @c compiler (text, default @c "d4").
 * Returns: jsonb object with the d-DNNF statistics.
 */
Datum ddnnf_stats(PG_FUNCTION_ARGS)
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

    // Time only the compilation: the treewidth probe below is a
    // separate, untimed introspection step.  "inversion-free" builds the
    // structured d-DNNF over the generic circuit's order keys (see
    // buildInversionFreeDDNNF) rather than the BooleanCircuit dispatch; the
    // treewidth probe below still runs on c, which is the point -- it shows the
    // structured d-DNNF stays small even where the treewidth is large.
    auto t0 = std::chrono::steady_clock::now();
    // Keeps the external-compilation default (this surface reports d-DNNF
    // compiler stats); the makeDD cost optimizer ('auto') is for shapley /
    // banzhaf, not the KC-compiler inspection surfaces.
    dDNNF d = (compiler == "inversion-free")
      ? buildInversionFreeDDNNF(*token)
      : c.makeDDByName(root, compiler);
    auto t1 = std::chrono::steady_clock::now();
    double compile_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

    dDNNF::Stats s = d.nodeStats();

    // Best-effort treewidth of the circuit's primal graph: informative
    // regardless of the chosen compiler, but may exceed the supported
    // limit, in which case we report null rather than failing.
    bool has_tw = false;
    unsigned treewidth = 0;
    try {
      TreeDecomposition td(c);
      treewidth = td.getTreewidth();
      has_tw = true;
    } catch(...) {
      has_tw = false;
    }

    std::ostringstream out;
    out << '{';
    out << "\"compiler\":\"";
    for(char ch : compiler)
      out << (ch == '"' || ch == '\\' ? std::string("\\") + ch : std::string(1, ch));
    out << "\"";
    out << ",\"nodes\":"  << s.nodes;
    out << ",\"edges\":"  << s.edges;
    out << ",\"and\":"    << s.and_gates;
    out << ",\"or\":"     << s.or_gates;
    out << ",\"not\":"    << s.not_gates;
    out << ",\"inputs\":" << s.inputs;
    out << ",\"smooth\":" << (s.smooth ? "true" : "false");
    out << ",\"depth\":"  << s.depth;
    out << ",\"treewidth\":";
    if(has_tw) out << treewidth; else out << "null";
    out << ",\"compile_ms\":" << compile_ms;
    out << '}';

    Datum json_datum = DirectFunctionCall1(
      jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
    PG_RETURN_DATUM(json_datum);
  } catch(const std::exception &e) {
    provsql_error("%s", e.what());
  } catch(...) {
    provsql_error("Unknown exception");
  }
  PG_RETURN_NULL();
}
