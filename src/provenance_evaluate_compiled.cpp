/**
 * @file provenance_evaluate_compiled.cpp
 * @brief SQL function @c provsql.provenance_evaluate_compiled() – C++ semiring evaluation.
 *
 * Implements the compiled (C++ generic) variant of provenance circuit
 * evaluation.  Unlike @c provenance_evaluate() (which calls user-supplied
 * PostgreSQL functions for each semiring operation), this function
 * evaluates the circuit using one of the built-in C++ semiring
 * implementations from the @c semiring/ directory.
 *
 * Supported semirings (selected by the @p semiring argument):
 * - @c "boolean"   → @c semiring::Boolean
 * - @c "counting"  → @c semiring::Counting
 * - @c "formula"   → @c semiring::Formula (symbolic representation as a formula)
 * - @c "how"       → @c semiring::How (canonical polynomial provenance ℕ[X])
 * - @c "why"       → @c semiring::Why (witness sets)
 * - @c "which"     → @c semiring::Which (lineage)
 * - @c "boolexpr"  → @c semiring::BoolExpr (Boolean circuit for probability)
 * - @c "tropical"  → @c semiring::Tropical (min-plus, shortest-cost)
 * - @c "viterbi"   → @c semiring::Viterbi (max-times, most-likely derivation)
 * - @c "lukasiewicz" → @c semiring::Lukasiewicz (Łukasiewicz fuzzy t-norm)
 * - @c "interval_union" → @c semiring::IntervalUnion (multirange union, PG14+)
 *   for any of <tt>tstzmultirange</tt>, <tt>nummultirange</tt>,
 *   <tt>int4multirange</tt>; selected by the result type of the call
 * - @c "minmax" / @c "maxmin" → @c semiring::MinMax over any user-defined
 *   PostgreSQL enum type, selected by @c get_typtype() == @c TYPTYPE_ENUM
 *
 * Each compiled semiring exposes @c parse_leaf() and (for text-valued
 * carriers) @c to_text() member functions, so this file is purely a
 * dispatcher: it picks the right semiring instance from the
 * (result-type, semiring-name) pair, then calls the @c pec() template
 * which runs the same four-step pipeline for every semiring (build leaf
 * mapping, evaluate HAVING sub-circuits, evaluate the main circuit,
 * encode the result as a Datum).
 */
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(provenance_evaluate_compiled);
}

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include "Expectation.h"
#include "having_semantics.hpp"
#include "provenance_evaluate_compiled.hpp"
#include "semiring/Boolean.h"
#include "semiring/Counting.h"
#include "semiring/Formula.h"
#include "semiring/How.h"
#include "semiring/Why.h"
#include "semiring/Which.h"
#include "semiring/BoolExpr.h"
#include "semiring/Tropical.h"
#include "semiring/Viterbi.h"
#include "semiring/Lukasiewicz.h"
#include "semiring/IntervalUnion.h"
#include "semiring/MinMax.h"

const char *drop_temp_table = "DROP TABLE IF EXISTS tmp_uuids;";

namespace {

/**
 * @brief Wrap a @c std::string in a Postgres @c text Datum.
 */
Datum text_datum(const std::string &s) {
  text *result = (text *) palloc(VARHDRSZ + s.size());
  SET_VARSIZE(result, VARHDRSZ + s.size());
  memcpy(VARDATA(result), s.c_str(), s.size());
  return PointerGetDatum(result);
}

// to_datum overloads: encode a semiring's evaluation result as a Postgres Datum.
Datum to_datum(const semiring::Boolean &, bool v)         { return BoolGetDatum(v); }
Datum to_datum(const semiring::Counting &, unsigned v)    { return Int32GetDatum(static_cast<int32>(v)); }
Datum to_datum(const semiring::Tropical &, double v)      { return Float8GetDatum(v); }
Datum to_datum(const semiring::Viterbi &, double v)       { return Float8GetDatum(v); }
Datum to_datum(const semiring::Lukasiewicz &, double v)   { return Float8GetDatum(v); }
Datum to_datum(const semiring::Formula &sr, const std::string &v) { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::Why &sr, const semiring::why_provenance_t &v)     { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::How &sr, const semiring::how_provenance_t &v)     { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::Which &sr, const semiring::which_provenance_t &v) { return text_datum(sr.to_text(v)); }
#if PG_VERSION_NUM >= 140000
Datum to_datum(const semiring::IntervalUnion &, Datum v)  { return v; }
#endif
Datum to_datum(const semiring::MinMax &, Datum v)         { return v; }

/**
 * @brief Run the four-step compiled-evaluation pipeline for a semiring.
 *
 * 1. Build the input-gate → leaf-value mapping by parsing each leaf via
 *    @c sr.parse_leaf().
 * 2. Rewrite HAVING comparison gates in @p c via @c provsql_having().
 * 3. Evaluate the main circuit at @p g.
 * 4. Encode the result as a Postgres Datum via @c to_datum(sr, ...).
 *
 * @tparam Sem        The compiled semiring class.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param sr          Semiring instance.
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            The semiring evaluation result encoded as a Datum.
 */
template <typename Sem>
Datum pec(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const Sem &sr,
  bool drop_table)
{
  using V = typename Sem::value_type;
  std::unordered_map<gate_t, V> mapping;
  initialize_provenance_mapping<V>(constants, c, mapping,
    [&sr](const char *v) { return sr.parse_leaf(v); }, drop_table);

  provsql_having(c, g, mapping, sr);
  V out = c.evaluate<Sem>(g, mapping, sr);
  return to_datum(sr, std::move(out));
}

/**
 * @brief Evaluate the BoolExpr semiring; bypasses the GenericCircuit pipeline.
 *
 * With @p labels, each input gate renders as its mapped label; without,
 * leaves render as the default @c x@<id@> placeholders.
 */
Datum pec_boolexpr(
  BooleanCircuit &bc,
  gate_t root,
  const std::unordered_map<gate_t, std::string> *labels)
{
  return text_datum(labels ? bc.toString(root, *labels) : bc.toString(root));
}

} // namespace

/**
 * @brief Join a provenance mapping table with a set of UUIDs using SPI.
 * @param table  OID of the provenance mapping relation.
 * @param uuids  List of UUID strings to join against.
 * @return       @c true if a temporary table was created (caller must drop it).
 */
bool join_with_temp_uuids(Oid table, const std::vector<std::string> &uuids) {
  if (SPI_connect() != SPI_OK_CONNECT)
    throw CircuitException("SPI_connect failed");

  char *table_name = get_rel_name(table);
  if (!table_name) {
    SPI_finish();
    throw CircuitException("Invalid OID: no such table");
  }
  // Schema-qualify so the lookup works regardless of search_path.
  // get_rel_name alone would resolve unqualified, which fails when the
  // mapping lives in a schema not on search_path (e.g. provsql_test in
  // the regression DB, or any user schema reached via Studio with the
  // default "public, provsql" search_path).
  const char *qualified = quote_qualified_identifier(
    get_namespace_name(get_rel_namespace(table)), table_name);

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
                     "SELECT value, provenance FROM %s WHERE 'f'", qualified);
  } else if(uuids.size() <= nb_max_uuid_value) {
    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM %s t JOIN (VALUES", qualified);
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
                     "SELECT value, provenance FROM %s t JOIN tmp_uuids u ON t.provenance = u.id", qualified);
  }

  if (SPI_exec(join_query.data, 0) != SPI_OK_SELECT) {
    if(drop_table)
      SPI_exec(drop_temp_table, 0);
    SPI_finish();
    throw CircuitException("Join query failed");
  }

  return drop_table;
}

/**
 * @brief Core implementation of compiled provenance evaluation.
 * @param token    UUID of the root provenance gate.
 * @param table    OID of the provenance mapping relation.
 * @param semiring Name of the semiring to evaluate over.
 * @param type     OID of the result element type.
 * @return         Datum containing the semiring evaluation result.
 */
static Datum provenance_evaluate_compiled_internal
  (pg_uuid_t token, Oid table, const std::string &semiring, Oid type)
{
  constants_t constants = get_constants(true);

  // boolexpr without a mapping has nothing to fetch from SPI: skip the
  // GenericCircuit build + temp-UUID join and read the BooleanCircuit
  // directly. Every other case (including boolexpr WITH a mapping)
  // shares the prelude below.
  if(semiring=="boolexpr" && !OidIsValid(table)) {
    gate_t root;
    BooleanCircuit bc = getBooleanCircuit(token, root);
    return pec_boolexpr(bc, root, nullptr);
  }

  // The expectation evaluator reads its leaves directly from the
  // gate_rv / gate_value extra fields; no leaf-mapping table is
  // involved.  Bypass the SPI prelude.
  if(semiring=="expectation") {
    if(type != constants.OID_TYPE_FLOAT)
      throw CircuitException(
              "Unknown element type for expectation: must be float8");
    GenericCircuit gc = getGenericCircuit(token);
    auto root = gc.getGate(uuid2string(token));
    return Float8GetDatum(provsql::compute_expectation(gc, root));
  }

  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  auto inputs = c.getInputs();

  std::vector<std::string> inputs_uuid;
  std::transform(inputs.begin(), inputs.end(), std::back_inserter(inputs_uuid), [&c](auto x) {
    return c.getUUID(x);
  });
  bool drop_table = join_with_temp_uuids(table, inputs_uuid);

  if(semiring=="boolexpr") {
    // boolexpr with a mapping: build the gate-to-label map on the
    // GenericCircuit (the mapping table is keyed by input UUIDs that
    // don't survive translation to BooleanCircuit gate ids), then
    // translate keys to bc gates via gc_to_bc so bc.toString can label
    // each leaf with its mapped value.
    gate_t root;
    std::unordered_map<gate_t, std::string> gc_labels;
    initialize_provenance_mapping<std::string>(constants, c, gc_labels, [](const char *v) {
      return std::string(v);
    }, drop_table);

    std::unordered_map<gate_t, gate_t> gc_to_bc;
    BooleanCircuit bc = getBooleanCircuit(c, token, root, gc_to_bc);

    std::unordered_map<gate_t, std::string> bc_labels;
    bc_labels.reserve(gc_labels.size());
    for(const auto &kv : gc_labels) {
      auto it = gc_to_bc.find(kv.first);
      if(it != gc_to_bc.end())
        bc_labels[it->second] = kv.second;
    }
    return pec_boolexpr(bc, root, &bc_labels);
  }

  if (type == constants.OID_TYPE_VARCHAR)
  {
    if (semiring == "formula") return pec(constants, c, g, semiring::Formula{}, drop_table);
    if (semiring == "why")     return pec(constants, c, g, semiring::Why{}, drop_table);
    if (semiring == "how")     return pec(constants, c, g, semiring::How{}, drop_table);
    if (semiring == "which")   return pec(constants, c, g, semiring::Which{}, drop_table);
    throw CircuitException("Unknown semiring for type varchar: " + semiring);
  }
  if (type == constants.OID_TYPE_INT) {
    if (semiring == "counting") return pec(constants, c, g, semiring::Counting{}, drop_table);
    throw CircuitException("Unknown semiring for type int: " + semiring);
  }
  if (type == constants.OID_TYPE_FLOAT) {
    if (semiring == "tropical")    return pec(constants, c, g, semiring::Tropical{}, drop_table);
    if (semiring == "viterbi")     return pec(constants, c, g, semiring::Viterbi{}, drop_table);
    if (semiring == "lukasiewicz") return pec(constants, c, g, semiring::Lukasiewicz{}, drop_table);
    throw CircuitException("Unknown semiring for type float: " + semiring);
  }
  if (type == constants.OID_TYPE_BOOL) {
    if (semiring == "boolean") return pec(constants, c, g, semiring::Boolean{}, drop_table);
    throw CircuitException("Unknown semiring for type bool: " + semiring);
  }
#if PG_VERSION_NUM >= 140000
  if (type == constants.OID_TYPE_TSTZMULTIRANGE) {
    if (semiring != "temporal" && semiring != "interval_union")
      throw CircuitException("Unknown semiring for type tstzmultirange: " + semiring);
    return pec(constants, c, g, semiring::IntervalUnion(constants.OID_TYPE_TSTZMULTIRANGE), drop_table);
  }
  if (type == constants.OID_TYPE_NUMMULTIRANGE) {
    if (semiring != "interval_union")
      throw CircuitException("Unknown semiring for type nummultirange: " + semiring);
    return pec(constants, c, g, semiring::IntervalUnion(constants.OID_TYPE_NUMMULTIRANGE), drop_table);
  }
  if (type == constants.OID_TYPE_INT4MULTIRANGE) {
    if (semiring != "interval_union")
      throw CircuitException("Unknown semiring for type int4multirange: " + semiring);
    return pec(constants, c, g, semiring::IntervalUnion(constants.OID_TYPE_INT4MULTIRANGE), drop_table);
  }
#endif
  if (get_typtype(type) == TYPTYPE_ENUM) {
    if (semiring == "minmax") return pec(constants, c, g, semiring::MinMax(type, false), drop_table);
    if (semiring == "maxmin") return pec(constants, c, g, semiring::MinMax(type, true), drop_table);
    throw CircuitException("Unknown semiring for enum type: " + semiring);
  }
  throw CircuitException("Unknown element type for provenance_evaluate_compiled");
}

/** @brief PostgreSQL-callable wrapper for provenance_evaluate_compiled(). */
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
    provsql_error("provenance_evaluate_compiled: %s", e.what());
  } catch(...) {
    provsql_error("provenance_evaluate_compiled: Unknown exception");
  }

  PG_RETURN_NULL();
}
