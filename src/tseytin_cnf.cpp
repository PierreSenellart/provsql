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
#include "utils/jsonb.h"
#include "utils/fmgrprotos.h"
#include "utils/uuid.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(tseytin_cnf);
PG_FUNCTION_INFO_V1(tseytin_cnf_mapping_json);
}

#include "c_cpp_compatibility.h"
#include "BooleanCircuit.h"
#include "CircuitFromMMap.h"
#include "provsql_utils_cpp.h"

#include <sstream>
#include <string>

using namespace std;

/**
 * @brief PostgreSQL-callable entry point.
 *
 * Arguments: @c token (uuid), @c weighted (bool, default true),
 * @c mapping (bool, default true).
 * Returns: DIMACS CNF as text; when @c weighted, includes @c w lines;
 * when @c mapping, prepends @c "c input <var> <uuid> <prob>" comments.
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

    bool mapping = true;
    if(!PG_ARGISNULL(2))
      mapping = PG_GETARG_BOOL(2);

    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    string cnf = c.TseytinCNF(root, weighted, mapping);

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

static void json_escape(std::ostringstream &out, const std::string &s)
{
  for(char ch : s) {
    switch(ch) {
    case '"':  out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    default:   out << ch;
    }
  }
}

/**
 * @brief PostgreSQL-callable entry point: variable mapping as jsonb.
 *
 * Argument: @c token (uuid). Returns a jsonb array of objects
 * @c {"variable":int,"gate":uuid-string,"probability":float8}, one per
 * input gate, with the same variable numbering as @c tseytin_cnf. The
 * SQL wrapper @c tseytin_cnf_mapping unnests this into a table.
 */
Datum tseytin_cnf_mapping_json(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));

  std::ostringstream out;
  out << '[';
  try {
    gate_t root;
    BooleanCircuit c = getBooleanCircuit(*token, root);
    c.rewriteMultivaluedGates();

    bool first = true;
    for(const auto &m : c.tseytinVariableMapping()) {
      if(!first) out << ',';
      first = false;
      out << "{\"variable\":" << m.variable << ",\"gate\":";
      if(m.uuid.empty()) {
        out << "null";
      } else {
        out << '"';
        json_escape(out, m.uuid);
        out << '"';
      }
      out << ",\"probability\":";
      if(m.probability != m.probability)  /* NaN */
        out << "null";
      else
        out << m.probability;
      out << '}';
    }
  } catch(const std::exception &e) {
    provsql_error("%s", e.what());
  } catch(...) {
    provsql_error("Unknown exception");
  }
  out << ']';

  Datum json_datum = DirectFunctionCall1(
    jsonb_in, CStringGetDatum(pstrdup(out.str().c_str())));
  PG_RETURN_DATUM(json_datum);
}
