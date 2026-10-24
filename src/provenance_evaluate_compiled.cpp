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
 * - @c "subset" / @c "clearance" / @c "consent" → @c semiring::Subset, the
 *   subsets of the labels of an enum type, returned as a @c bigint bitmask
 *   (declared in SQL as @c subset_evaluate)
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
#include "utils/memutils.h"
#include "utils/datum.h"
#include "utils/array.h"
#include "access/xact.h"
#include "access/htup_details.h"
#include "funcapi.h"
#include "compatibility.h"   /* TYPALIGN_CHAR fallback for PG < 13 */
#include "provsql_utils.h"
#include "provsql_mmap.h"
#include "agg_token.h"

PG_FUNCTION_INFO_V1(provenance_evaluate_compiled);
PG_FUNCTION_INFO_V1(provenance_evaluate_tropical_paths);
PG_FUNCTION_INFO_V1(provenance_evaluate_best_path);
PG_FUNCTION_INFO_V1(plain_truth);
PG_FUNCTION_INFO_V1(subset_labels);
PG_FUNCTION_INFO_V1(subset_settling);
PG_FUNCTION_INFO_V1(subset_member);
PG_FUNCTION_INFO_V1(sr_formula_rows);
}

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_set>

#include "CertifiedDDMaterialize.h"
#include "CircuitFromMMap.h"
#include "Expectation.h"
#include "having_semantics.hpp"
#include "provenance_evaluate_compiled.hpp"
#include "provsql_interrupt.h"
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
#include "semiring/Subset.h"
#include "semiring/Regex.h"
#include "semiring/TropicalPaths.h"
#include "semiring/BestPath.h"

const char *drop_temp_table = "DROP TABLE IF EXISTS tmp_uuids;";

namespace {

/**
 * @brief Wrap a @c std::string in a Postgres @c text Datum.
 *
 * Within @c provsql.max_memory, the copy counted, and within what a text
 * value holds (1 GB): a 54000 error otherwise, not palloc's internal one.
 */
Datum text_datum(const std::string &s) {
  provsql_check_result_size(
    s.size(), "the result is too large to return (over 1 GB): a sub-circuit "
    "shared by several gates is written out at each of them");
  provsql_check_memory_for(s.size());
  text *result = (text *) palloc(VARHDRSZ + s.size());
  SET_VARSIZE(result, VARHDRSZ + s.size());
  memcpy(VARDATA(result), s.c_str(), s.size());
  return PointerGetDatum(result);
}

// to_datum overloads: encode a semiring's evaluation result as a Postgres Datum.
Datum to_datum(const semiring::Boolean &, bool v)         { return BoolGetDatum(v); }
Datum to_datum(const semiring::Counting &, unsigned v) {
  if(v == semiring::Counting::INFINITE)
    throw CircuitRefusal(
            PROVSQL_DELIBERATE, "recursion-infinite-count",
            "This tuple has infinitely many derivations (it is derived "
            "through a cycle of its recursive query), which an integer "
            "count cannot represent.");
  return Int32GetDatum(static_cast<int32>(v));
}
Datum to_datum(const semiring::Tropical &, double v)      { return Float8GetDatum(v); }
Datum to_datum(const semiring::Viterbi &, double v)       { return Float8GetDatum(v); }
Datum to_datum(const semiring::Lukasiewicz &, double v)   { return Float8GetDatum(v); }
Datum to_datum(const semiring::Formula &sr, std::string v) { return text_datum(sr.to_text(std::move(v))); }
Datum to_datum(const semiring::Why &sr, const semiring::why_provenance_t &v)     { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::How &sr, const semiring::how_provenance_t &v)     { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::Which &sr, const semiring::which_provenance_t &v) { return text_datum(sr.to_text(v)); }
Datum to_datum(const semiring::Regex &sr, const semiring::regex_t &v) { return text_datum(sr.to_text(v)); }
#if PG_VERSION_NUM >= 140000
Datum to_datum(const semiring::IntervalUnion &, Datum v)  { return v; }
#endif
Datum to_datum(const semiring::MinMax &, Datum v)         { return v; }
Datum to_datum(const semiring::Subset &, uint64_t v)     { return Int64GetDatum(static_cast<int64>(v)); }

namespace {

/* Solutions of equation systems (recursive queries lowered by
 * eval_recursive_system), one Datum per component, under the key
 * "<context>|<fixpoint token>".  The context names the semiring, the
 * mapping and the result type, and the current command, so that a change
 * to the mapping in the same transaction is never served a stale value;
 * the cache lives until the end of the transaction.  A query evaluating
 * every row of a recursive result solves its system once, not once per
 * row. */
std::unordered_map<std::string, Datum> fixpoint_cache;
MemoryContext fixpoint_cache_cxt = nullptr;
bool fixpoint_cache_callback = false;
constexpr size_t fixpoint_cache_max = 4000000;

/* Context of the evaluation in progress, empty when not cacheable. */
std::string fixpoint_cache_ctx;
int16 fixpoint_cache_typlen;
bool fixpoint_cache_typbyval;

/* The same, for sr_tropical_paths, whose (cost, paths) results are no
 * Datum of one type: kept as they are, under the key
 * "<costs>|<labels>|<command>|<fixpoint token>". */
std::unordered_map<std::string, std::pair<double, std::string> >
tropical_paths_cache;

/* The same, for sr_best_path. */
std::unordered_map<std::string, semiring::best_path_t> best_path_cache;

void fixpoint_cache_clear()
{
  fixpoint_cache.clear();
  tropical_paths_cache.clear();
  best_path_cache.clear();
  if(fixpoint_cache_cxt)
    MemoryContextReset(fixpoint_cache_cxt);
}

void fixpoint_cache_xact_callback(XactEvent event, void *)
{
  if(event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT
     || event == XACT_EVENT_PARALLEL_COMMIT
     || event == XACT_EVENT_PARALLEL_ABORT
     || event == XACT_EVENT_PREPARE)
    fixpoint_cache_clear();
}

/* Store the solution of every system @p c solved, under the context set
 * by the caller. */
template <typename Sem>
void fixpoint_cache_store(const GenericCircuit &c, const Sem &sr)
{
  if(fixpoint_cache_ctx.empty() || c.solved_systems.empty())
    return;
  if(!fixpoint_cache_cxt) {
    fixpoint_cache_cxt = AllocSetContextCreate(TopMemoryContext,
                                               "ProvSQL fixpoint cache",
                                               ALLOCSET_DEFAULT_SIZES);
  }
  if(!fixpoint_cache_callback) {
    RegisterXactCallback(fixpoint_cache_xact_callback, nullptr);
    fixpoint_cache_callback = true;
  }
  using V = typename Sem::value_type;
  for(const auto &[sys, p] : c.solved_systems) {
    const auto &vec = *static_cast<const std::vector<V> *>(p.get());
    if(fixpoint_cache.size() + vec.size() > fixpoint_cache_max)
      fixpoint_cache_clear();
    const std::string prefix = "fixpoint:" + c.getUUID(sys) + ":";
    for(std::size_t i = 0; i < vec.size(); ++i) {
      const std::string tok =
        uuid2string(provsqlUuidV5(prefix + std::to_string(i + 1)));
      /* A component with no value in the result type (an infinite count)
       * is not cached: the row reading it is refused on its own. */
      Datum d;
      try {
        d = to_datum(sr, vec[i]);
      } catch(const CircuitException &) {
        /* A resource limit stops the evaluation, not just this entry */
        if(provsql_limit_pending())
          throw;
        continue;
      }
      MemoryContext old = MemoryContextSwitchTo(fixpoint_cache_cxt);
      d = datumCopy(d, fixpoint_cache_typbyval, fixpoint_cache_typlen);
      MemoryContextSwitchTo(old);
      fixpoint_cache.emplace(fixpoint_cache_ctx + "|" + tok, d);
    }
  }
}

}

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
  c.evaluate<Sem>(g, mapping, sr);
  /* The result moved out of the values of the gates, not copied */
  V out = std::move(mapping.at(g));
  fixpoint_cache_store(c, sr);
  /* The value of every gate, freed before the result is copied: a
   * formula's would otherwise hold several times its size. */
  std::unordered_map<gate_t, V>().swap(mapping);
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

  // No mapping given: run an empty join, so the provenance mapping stays
  // empty and every leaf falls back to the semiring's one (the
  // absent-mapping convention).
  if (!OidIsValid(table)) {
    if (SPI_execute("SELECT ''::text AS value, NULL::uuid AS provenance WHERE false",
                    true, 0) != SPI_OK_SELECT) {
      SPI_finish();
      throw CircuitException("Join query failed");
    }
    return false;
  }

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

  // The UUIDs as a uuid[] parameter, when there are at most
  // nb_max_uuid_value of them; a parameter rather than a literal list keeps
  // the query text short, which is also what an error or a cancellation
  // reports as its context.
  Datum uuid_array = (Datum) 0;

  // Two different mechanisms to implement the join (unless there are no
  // UUIDs):
  // if there are less than nb_max_uuid_value UUIDs, we join with the
  // unnested uuid[] parameter;
  // otherwise we create a temporary table where we dump the inserts
  // and join with it.
  if(uuids.size() == 0) {
    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM %s WHERE 'f'", qualified);
  } else if(uuids.size() <= nb_max_uuid_value) {
    pg_uuid_t *values =
      static_cast<pg_uuid_t *>(palloc(uuids.size() * sizeof(pg_uuid_t)));
    Datum *elems = static_cast<Datum *>(palloc(uuids.size() * sizeof(Datum)));
    for(size_t i = 0; i < uuids.size(); ++i) {
      values[i] = string2uuid(uuids[i]);
      elems[i] = UUIDPGetDatum(&values[i]);
    }
    uuid_array = PointerGetDatum(
      construct_array(elems, uuids.size(), UUIDOID, UUID_LEN, false,
                      TYPALIGN_CHAR));
    pfree(elems);
    pfree(values);

    appendStringInfo(&join_query,
                     "SELECT value, provenance FROM %s t "
                     "JOIN unnest($1) AS u(id) ON t.provenance=u.id",
                     qualified);
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

  /* Read-only unless it reads the temporary table just filled: an sr_*
   * evaluation is PARALLEL SAFE, and a parallel worker can only run a
   * read-only command. */
  Oid argtypes[1] = {UUIDARRAYOID};
  const int ret = uuid_array != (Datum) 0
    ? SPI_execute_with_args(join_query.data, 1, argtypes, &uuid_array,
                            nullptr, true, 0)
    : SPI_execute(join_query.data, !drop_table, 0);
  if (ret != SPI_OK_SELECT) {
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
    applyProbabilisticSimplification(gc);
    auto root = gc.getGate(uuid2string(token));
    return Float8GetDatum(provsql::compute_expectation(gc, root));
  }

  /* A component of an equation system solved earlier in this command,
   * under the same semiring and mapping: no circuit to load. */
  fixpoint_cache_ctx = semiring + "|" + std::to_string(table) + "|" +
                       std::to_string(type) + "|" +
                       std::to_string(GetCurrentCommandId(false));
  /* The subset semirings answer a bigint bitmask, whatever the enum type of
   * the sample element that selects them. */
  if(semiring == "subset" || semiring == "clearance" || semiring == "consent")
    get_typlenbyval(INT8OID, &fixpoint_cache_typlen, &fixpoint_cache_typbyval);
  else
    get_typlenbyval(type, &fixpoint_cache_typlen, &fixpoint_cache_typbyval);
  {
    auto it = fixpoint_cache.find(fixpoint_cache_ctx + "|" + uuid2string(token));
    if(it != fixpoint_cache.end())
      return datumCopy(it->second, fixpoint_cache_typbyval,
                       fixpoint_cache_typlen);
  }

  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  bool drop_table = join_with_temp_uuids(table, c.getMappableUUIDs());

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
    if (semiring == "regex")   return pec(constants, c, g, semiring::Regex{}, drop_table);
    throw CircuitException("Unknown semiring for type varchar: " + semiring);
  }
  if (type == constants.OID_TYPE_INT) {
    if (semiring == "counting") return pec(constants, c, g, semiring::Counting{}, drop_table);
    throw CircuitException("Unknown semiring for type int: " + semiring);
  }
  if (type == constants.OID_TYPE_FLOAT) {
    if (semiring == "tropical")    return pec(constants, c, g, semiring::Tropical{}, drop_table);
    if (semiring == "tropical_nonneg")
      return pec(constants, c, g, semiring::TropicalNonneg{}, drop_table);
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
    if (semiring == "subset")
      return pec(constants, c, g, semiring::Subset(type, semiring::Subset::Mode::SUBSET), drop_table);
    if (semiring == "clearance")
      return pec(constants, c, g, semiring::Subset(type, semiring::Subset::Mode::CLEARANCE), drop_table);
    if (semiring == "consent")
      return pec(constants, c, g, semiring::Subset(type, semiring::Subset::Mode::CONSENT), drop_table);
    throw CircuitException("Unknown semiring for enum type: " + semiring);
  }
  throw CircuitException("Unknown element type for provenance_evaluate_compiled");
}

/**
 * @brief Min-cost derivations of @p token and the derivations of that cost
 *        (@c semiring::TropicalPaths), each leaf with its cost from
 *        @p costs and its label from @p labels.
 *
 * An input missing from @p costs costs 0, one missing from @p labels is
 * the empty word.
 *
 * @return The cost (@f$+\infty@f$ with no derivation) and the derivations,
 *         as a regular expression.
 */
static std::pair<double, std::string> tropical_paths_internal
  (pg_uuid_t token, Oid costs, Oid labels)
{
  constants_t constants = get_constants(true);
  fixpoint_cache_ctx.clear();

  /* A component of an equation system solved earlier in this command,
   * with the same mappings: no circuit to load. */
  const std::string ctx = std::to_string(costs) + "|" +
                          std::to_string(labels) + "|" +
                          std::to_string(GetCurrentCommandId(false));
  {
    auto it = tropical_paths_cache.find(ctx + "|" + uuid2string(token));
    if(it != tropical_paths_cache.end())
      return it->second;
  }

  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  const std::vector<std::string> inputs_uuid = c.getMappableUUIDs();

  std::unordered_map<gate_t, double> cost;
  initialize_provenance_mapping<double>(constants, c, cost,
    [](const char *v) { return atof(v); },
    join_with_temp_uuids(costs, inputs_uuid));
  std::unordered_map<gate_t, std::string> label;
  initialize_provenance_mapping<std::string>(constants, c, label,
    [](const char *v) { return std::string(v); },
    join_with_temp_uuids(labels, inputs_uuid));

  semiring::TropicalPaths sr;
  std::unordered_map<gate_t, semiring::tropical_paths_t> mapping;
  std::set<gate_t> leaves(c.getInputs());
  for(const auto &kv : cost)
    leaves.insert(kv.first);
  for(const auto &kv : label)
    leaves.insert(kv.first);
  for(auto x : leaves) {
    auto ic = cost.find(x);
    auto il = label.find(x);
    mapping[x] = semiring::tropical_paths_t{
      ic == cost.end() ? 0. : ic->second,
      semiring::regex_ops::atom(il == label.end() ? "" : il->second)};
  }

  provsql_having(c, g, mapping, sr);
  const auto out = c.evaluate<semiring::TropicalPaths>(g, mapping, sr);

  if(!c.solved_systems.empty() && !fixpoint_cache_callback) {
    RegisterXactCallback(fixpoint_cache_xact_callback, nullptr);
    fixpoint_cache_callback = true;
  }
  for(const auto &[sys, p] : c.solved_systems) {
    const auto &vec =
      *static_cast<const std::vector<semiring::tropical_paths_t> *>(p.get());
    if(tropical_paths_cache.size() + vec.size() > fixpoint_cache_max)
      tropical_paths_cache.clear();
    const std::string prefix = "fixpoint:" + c.getUUID(sys) + ":";
    for(std::size_t i = 0; i < vec.size(); ++i)
      tropical_paths_cache.emplace(
        ctx + "|" +
        uuid2string(provsqlUuidV5(prefix + std::to_string(i + 1))),
        std::make_pair(vec[i].cost, vec[i].paths.render()));
  }

  return {out.cost, out.paths.render()};
}

/** @brief PostgreSQL-callable wrapper for tropical_paths_internal(). */
Datum provenance_evaluate_tropical_paths(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();

  try {
    provsql_interrupt_scope interrupt_scope;
    const Oid costs = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
    const Oid labels = PG_ARGISNULL(2) ? InvalidOid : PG_GETARG_OID(2);
    const auto [cost, paths] =
      tropical_paths_internal(*DatumGetUUIDP(PG_GETARG_DATUM(0)), costs, labels);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      throw CircuitException("tropical paths must return a record");
    tupdesc = BlessTupleDesc(tupdesc);
    Datum values[2] = {Float8GetDatum(cost), text_datum(paths)};
    bool nulls[2] = {false, false};
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch(const CircuitRefusal &r) {
    provsql_cancel_if_interrupted();
    provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_tropical_paths: %s", e.what());
  } catch(...) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_tropical_paths: Unknown exception");
  }

  PG_RETURN_NULL();
}

/**
 * @brief A least-cost derivation of @p token (@c semiring::BestPath), each
 *        leaf with its cost from @p costs and its label from @p labels.
 *
 * An input missing from @p costs costs 0, one missing from @p labels is
 * the empty word.
 *
 * @return The cost (@f$+\infty@f$ with no derivation) and the labels of
 *         the derivation, in order.
 */
static semiring::best_path_t best_path_internal
  (pg_uuid_t token, Oid costs, Oid labels)
{
  constants_t constants = get_constants(true);
  fixpoint_cache_ctx.clear();

  /* A component of an equation system solved earlier in this command,
   * with the same mappings: no circuit to load. */
  const std::string ctx = std::to_string(costs) + "|" +
                          std::to_string(labels) + "|" +
                          std::to_string(GetCurrentCommandId(false));
  {
    auto it = best_path_cache.find(ctx + "|" + uuid2string(token));
    if(it != best_path_cache.end())
      return it->second;
  }

  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  const std::vector<std::string> inputs_uuid = c.getMappableUUIDs();

  std::unordered_map<gate_t, double> cost;
  initialize_provenance_mapping<double>(constants, c, cost,
    [](const char *v) { return atof(v); },
    join_with_temp_uuids(costs, inputs_uuid));
  std::unordered_map<gate_t, std::string> label;
  initialize_provenance_mapping<std::string>(constants, c, label,
    [](const char *v) { return std::string(v); },
    join_with_temp_uuids(labels, inputs_uuid));

  semiring::BestPath sr;
  std::unordered_map<gate_t, semiring::best_path_t> mapping;
  std::set<gate_t> leaves(c.getInputs());
  for(const auto &kv : cost)
    leaves.insert(kv.first);
  for(const auto &kv : label)
    leaves.insert(kv.first);
  for(auto x : leaves) {
    auto ic = cost.find(x);
    auto il = label.find(x);
    mapping[x] = semiring::BestPath::leaf(
      ic == cost.end() ? 0. : ic->second,
      il == label.end() ? nullptr : &il->second);
  }

  provsql_having(c, g, mapping, sr);
  const auto out = c.evaluate<semiring::BestPath>(g, mapping, sr);

  if(!c.solved_systems.empty() && !fixpoint_cache_callback) {
    RegisterXactCallback(fixpoint_cache_xact_callback, nullptr);
    fixpoint_cache_callback = true;
  }
  for(const auto &[sys, p] : c.solved_systems) {
    const auto &vec =
      *static_cast<const std::vector<semiring::best_path_t> *>(p.get());
    if(best_path_cache.size() + vec.size() > fixpoint_cache_max)
      best_path_cache.clear();
    const std::string prefix = "fixpoint:" + c.getUUID(sys) + ":";
    for(std::size_t i = 0; i < vec.size(); ++i)
      best_path_cache.emplace(
        ctx + "|" +
        uuid2string(provsqlUuidV5(prefix + std::to_string(i + 1))),
        vec[i]);
  }

  return out;
}

/** @brief PostgreSQL-callable wrapper for best_path_internal(). */
Datum provenance_evaluate_best_path(PG_FUNCTION_ARGS)
{
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();

  try {
    provsql_interrupt_scope interrupt_scope;
    const Oid costs = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
    const Oid labels = PG_ARGISNULL(2) ? InvalidOid : PG_GETARG_OID(2);
    const semiring::best_path_t out =
      best_path_internal(*DatumGetUUIDP(PG_GETARG_DATUM(0)), costs, labels);

    TupleDesc tupdesc;
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      throw CircuitException("best path must return a record");
    tupdesc = BlessTupleDesc(tupdesc);
    Datum values[2] = {Float8GetDatum(out.cost), (Datum)0};
    bool nulls[2] = {false, out.is_zero()};
    if(!out.is_zero()) {
      std::vector<Datum> elems;
      for(const auto &l : out.word)
        elems.push_back(text_datum(l));
      values[1] = PointerGetDatum(
        construct_array(elems.data(), elems.size(), TEXTOID, -1, false,
                        TYPALIGN_INT));
    }
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  } catch(const CircuitRefusal &r) {
    provsql_cancel_if_interrupted();
    provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_best_path: %s", e.what());
  } catch(...) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_best_path: Unknown exception");
  }

  PG_RETURN_NULL();
}

/** @brief PostgreSQL-callable wrapper for provenance_evaluate_compiled(). */
Datum provenance_evaluate_compiled(PG_FUNCTION_ARGS)
{
  /* The sr_* SQL wrappers are STRICT, but the function itself is not:
   * guard the required arguments rather than rely on the wrappers.  The
   * mapping (argument 1) is legitimately NULL -- the no-mapping mode
   * renders anonymous leaves -- and reads as InvalidOid below. */
  if (PG_ARGISNULL(0) || PG_ARGISNULL(2))
    PG_RETURN_NULL();

  try {
    provsql_interrupt_scope interrupt_scope;
    Datum token = PG_GETARG_DATUM(0);

    Oid table = PG_GETARG_OID(1);

    text *t = PG_GETARG_TEXT_P(2);
    std::string semiring(VARDATA(t),VARSIZE(t)-VARHDRSZ);

    Oid type = get_fn_expr_argtype(fcinfo->flinfo, 3);

    return provenance_evaluate_compiled_internal(*DatumGetUUIDP(token), table, semiring, type);
  } catch(const provsql::NoValueException &) {
    /* An expectation defined in no world: SQL's NULL. */
    provsql_cancel_if_interrupted();
    PG_RETURN_NULL();
  } catch(const CircuitRefusal &r) {
    provsql_cancel_if_interrupted();
    provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_compiled: %s", e.what());
  } catch(...) {
    provsql_cancel_if_interrupted();
    provsql_error("provenance_evaluate_compiled: Unknown exception");
  }

  PG_RETURN_NULL();
}

namespace {

/** Truth of the gates already met, in the database as it is: a gate never
 *  changes once created, nor does an input tuple's presence for the rows
 *  that read it.  Bounded, so that a long session does not grow it forever. */
std::unordered_map<std::string, bool> plain_truth_memo;
constexpr size_t plain_truth_memo_max = 1000000;

/** 1 / 0 for a gate whose truth follows from its type and its children's,
 *  -1 for one that needs the full evaluation (a comparison, ...). */
int quick_plain_truth(const pg_uuid_t &token, unsigned depth)
{
  std::string key = uuid2string(token);
  auto it = plain_truth_memo.find(key);
  if (it != plain_truth_memo.end())
    return it->second;
  if (depth > 64)
    return -1;

  unsigned nb_children = 0;
  pg_uuid_t *children = NULL;
  gate_type type = provsql_fetch_gate(&token, &nb_children, &children);
  int res = -1;

  switch (type) {
  case gate_input: case gate_mulinput: case gate_one:
    res = 1;
    break;
  case gate_zero:
    res = 0;
    break;
  case gate_times: case gate_plus: {
    bool is_times = type == gate_times;
    res = is_times ? 1 : 0;
    for (unsigned i = 0; i < nb_children; ++i) {
      int c = quick_plain_truth(children[i], depth + 1);
      if (c < 0) { res = -1; break; }
      if (is_times && !c) { res = 0; break; }
      if (!is_times && c) { res = 1; break; }
    }
    break;
  }
  case gate_monus:
    if (nb_children == 2) {
      int a = quick_plain_truth(children[0], depth + 1);
      int b = a == 1 ? quick_plain_truth(children[1], depth + 1) : 0;
      res = a < 0 || b < 0 ? -1 : (a && !b);
    }
    break;
  case gate_delta: case gate_project: case gate_eq: case gate_annotation:
  case gate_assumed:
    if (nb_children >= 1)
      res = quick_plain_truth(children[0], depth + 1);
    break;
  case gate_cmp: {
    /* A comparison of aggregate results holds, in the database as it is, by
     * the values its sides record there: agg_guard_holds reads them off the
     * gates.  Deciding it that way rather than over the Boolean semiring
     * matters, not merely for speed: the semiring reads a comparison through
     * the worlds of what it aggregates, which is 2^n terms for n
     * contributions, so a comparison over a few dozen rows -- one per row an
     * exploded aggregate value produces -- would not finish.  A comparison
     * whose sides record no value (a random variable, a text aggregate)
     * yields NULL and takes the evaluation below. */
    constants_t constants = get_constants(false);
    if (!constants.ok || !OidIsValid(constants.OID_FUNCTION_AGG_GUARD_HOLDS))
      break;
    {
      FmgrInfo flinfo;
      /* PostgreSQL 12 made the argument array variable-length, and with it
       * LOCAL_FCINFO and fcinfo->args[]; before that the call info is a plain
       * struct with fixed arg / argnull arrays. */
#if PG_VERSION_NUM >= 120000
      LOCAL_FCINFO(fcinfo, 1);
#else
      FunctionCallInfoData fcinfo_data;
      FunctionCallInfo fcinfo = &fcinfo_data;
#endif
      Datum d;

      fmgr_info(constants.OID_FUNCTION_AGG_GUARD_HOLDS, &flinfo);
      InitFunctionCallInfoData(*fcinfo, &flinfo, 1, InvalidOid, NULL, NULL);
#if PG_VERSION_NUM >= 120000
      fcinfo->args[0].value = UUIDPGetDatum(&token);
      fcinfo->args[0].isnull = false;
#else
      fcinfo->arg[0] = UUIDPGetDatum(&token);
      fcinfo->argnull[0] = false;
#endif
      d = FunctionCallInvoke(fcinfo);
      if (!fcinfo->isnull)
        res = DatumGetBool(d) ? 1 : 0;
    }
    break;
  }
  default:
    break;
  }
  free(children);

  if (res >= 0) {
    if (plain_truth_memo.size() >= plain_truth_memo_max)
      plain_truth_memo.clear();
    plain_truth_memo.emplace(std::move(key), res == 1);
  }
  return res;
}

} // namespace

/**
 * @brief Whether a token holds in the database as it is, every input tuple
 *        present: @c sr_boolean without a mapping.
 *
 * The rewriter filters by it the rows a displayed aggregate value reads, so
 * that the value is what plain SQL computes on the same data: a row kept only
 * for its provenance (the padding of a lowered outer join, a group a
 * @c HAVING rejects, a row beyond a rank-filtered @c LIMIT) does not count.
 * Gates whose truth follows from their children's (⊗, ⊕, ⊖, δ, ...) are
 * walked directly, through the backend's gate cache; a comparison is left to
 * the evaluation over the Boolean semiring, with no mapping to read.
 */
Datum plain_truth(PG_FUNCTION_ARGS)
{
  pg_uuid_t token;

  if (PG_ARGISNULL(0))
    PG_RETURN_BOOL(true);
  token = *PG_GETARG_UUID_P(0);

  int quick = quick_plain_truth(token, 0);
  if (quick >= 0)
    PG_RETURN_BOOL(quick == 1);

  /* A token the Boolean semiring cannot evaluate (a comparison on a random
   * variable, which has no value in the database as it is) holds: the row
   * counts, as it did before this filter. */
  bool res = true;
  try {
    provsql_interrupt_scope interrupt_scope;
    GenericCircuit c = getGenericCircuit(token);
    gate_t g = c.getGate(uuid2string(token));
    semiring::Boolean sr;
    std::unordered_map<gate_t, bool> mapping;
    provsql_having(c, g, mapping, sr);
    res = c.evaluate<semiring::Boolean>(g, mapping, sr);
    /* An equation system met on the way was solved whole: record the
     * truth of each of its components, so that the other rows of the same
     * recursive result, which the rewriter checks one by one, do not load
     * and solve it again. */
    for(const auto &[sys, p] : c.solved_systems) {
      const auto &vec = *static_cast<const std::vector<bool> *>(p.get());
      if(plain_truth_memo.size() + vec.size() >= plain_truth_memo_max)
        plain_truth_memo.clear();
      const std::string prefix = "fixpoint:" + c.getUUID(sys) + ":";
      for(std::size_t i = 0; i < vec.size(); ++i)
        plain_truth_memo.emplace(
          uuid2string(provsqlUuidV5(prefix + std::to_string(i + 1))), vec[i]);
    }
  } catch(const std::exception &) {
    provsql_cancel_if_interrupted();
    res = true;
  }
  if (plain_truth_memo.size() >= plain_truth_memo_max)
    plain_truth_memo.clear();
  plain_truth_memo.emplace(uuid2string(token), res);
  PG_RETURN_BOOL(res);
}

/**
 * @brief The labels a subset bitmask holds, as an array of the enum type of
 *        @p element_one, in @c enumsortorder.
 *
 * Bits above the last label (the unrestricted element of the consent
 * semiring) are ignored.
 */
Datum subset_labels(PG_FUNCTION_ARGS)
{
  /* Not STRICT: the enum argument is read for its type only, and may be
   * NULL. */
  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();
  uint64_t mask = static_cast<uint64_t>(PG_GETARG_INT64(0));
  Oid enum_oid = get_fn_expr_argtype(fcinfo->flinfo, 1);
  semiring::EnumLabels labels(enum_oid);
  std::vector<Datum> elems;
  for(std::size_t i = 0; i < labels.oids.size() && i < 63; ++i)
    if(mask & (uint64_t(1) << i))
      elems.push_back(ObjectIdGetDatum(labels.oids[i]));
  int16 typlen;
  bool typbyval;
  char typalign;
  get_typlenbyvalalign(enum_oid, &typlen, &typbyval, &typalign);
  PG_RETURN_ARRAYTYPE_P(construct_array(elems.data(), elems.size(), enum_oid,
                                        typlen, typbyval, typalign));
}

/**
 * @brief The settling level of a set of clearance levels: the lowest level
 *        from which membership no longer changes up to the top level.
 *
 * Settledness is upward-closed, so it always exists; the membership it
 * settles to is that of the top level.
 */
Datum subset_settling(PG_FUNCTION_ARGS)
{
  /* Not STRICT: the enum argument is read for its type only, and may be
   * NULL. */
  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();
  uint64_t mask = static_cast<uint64_t>(PG_GETARG_INT64(0));
  semiring::EnumLabels labels(get_fn_expr_argtype(fcinfo->flinfo, 1));
  std::size_t n = labels.oids.size();
  if(n == 0 || n > 63)
    provsql_error("subset_settling: an enum type of 1 to 63 labels is required");
  bool top = mask & (uint64_t(1) << (n - 1));
  std::size_t l = n - 1;
  while(l > 0 && static_cast<bool>(mask & (uint64_t(1) << (l - 1))) == top)
    --l;
  PG_RETURN_OID(labels.oids[l]);
}

/**
 * @brief Membership of a label, given as text, in the subset annotation of
 *        a token: the predicate behind @c visible_at and @c consented_for.
 *
 * The enum type is that of the mapping table's @c value column (or the
 * element type of an array column), so that the caller writes the label as
 * a plain string.  In @c consent mode, a purpose holds only where the
 * unrestricted element does too.
 */
Datum subset_member(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
    PG_RETURN_NULL();
  pg_uuid_t token = *PG_GETARG_UUID_P(0);
  Oid table = PG_GETARG_OID(1);
  text *m = PG_GETARG_TEXT_P(2);
  std::string mode(VARDATA(m), VARSIZE(m) - VARHDRSZ);
  char *label_text = text_to_cstring(PG_GETARG_TEXT_P(3));

  AttrNumber att = get_attnum(table, "value");
  if(att == InvalidAttrNumber)
    provsql_error("%s: the mapping has no column named value",
                  mode == "consent" ? "consented_for" : "visible_at");
  Oid coltype = get_atttype(table, att);
  Oid elemtype = get_element_type(coltype);
  Oid enum_oid = OidIsValid(elemtype) ? elemtype : coltype;
  if(get_typtype(enum_oid) != TYPTYPE_ENUM)
    provsql_error("%s: the value column of the mapping is not of an enum type",
                  mode == "consent" ? "consented_for" : "visible_at");

  Oid in_func, typioparam;
  getTypeInputInfo(enum_oid, &in_func, &typioparam);
  Oid label = DatumGetObjectId(OidInputFunctionCall(in_func, label_text,
                                                    typioparam, -1));
  semiring::EnumLabels labels(enum_oid);
  uint64_t bit = uint64_t(1) << labels.rank.at(label);

  uint64_t mask = 0;
  try {
    provsql_interrupt_scope interrupt_scope;
    mask = static_cast<uint64_t>(DatumGetInt64(
      provenance_evaluate_compiled_internal(token, table, mode, enum_oid)));
  } catch(const CircuitRefusal &r) {
    provsql_cancel_if_interrupted();
    provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("subset_member: %s", e.what());
  }
  if(mode == "consent")
    PG_RETURN_BOOL((mask & bit) && (mask & semiring::Subset::UNRESTRICTED));
  PG_RETURN_BOOL(mask & bit);
}

namespace {

/**
 * @brief The rows of @c sr_formula_rows, produced one at a time.
 *
 * The circuit is not loaded whole: each row is written from its region,
 * the gates below its own down to the named ones, which the store answers
 * (@c FormulaRegions); the rows of the children of a wide gate are asked
 * for @c BATCH at a time.  A row thus costs what it writes, and the memory
 * used does not depend on the size of the circuit.
 *
 * The size of a formula is counted in symbols, a gate for one, written out
 * at each use, a named descendant for one.  A gate of more than
 * @c max_size is named (@c #1, @c #2, ...), the root always; its row is
 * written with its named descendants as their names.  A named gate whose
 * row would still exceed @c SLACK times @c max_size (a sum of millions of
 * small contributions) is summarised in its row, each child given a row of
 * its own (@c #k.1, @c #k.2, ...).
 *
 * The comparisons on aggregates in a region are written as @c sr_formula
 * writes them where that fits in a row, as they are otherwise (see
 * @c expand_comparisons).
 */
class FormulaRows {
  constants_t constants;
  Oid table;
  std::size_t max_size;
  /** Names of the named gates met so far, by UUID */
  std::unordered_map<std::string, std::string> names;
  std::size_t next_name = 1;
  /** A gate's own row (@c child unset), or the row of the child of a wide
   *  gate at position @c child, out of @c nb */
  struct Item {
    std::string uuid;
    std::size_t child;
    std::size_t nb;
  };
  static constexpr std::size_t OWN = std::numeric_limits<std::size_t>::max();
  /** How much longer than @c max_size the row of a named gate may be
   *  before it is split into one row per child */
  static constexpr std::size_t SLACK = 4;
  /** Rows of the children of a wide gate asked for at once */
  static constexpr std::size_t BATCH = 4096;
  std::deque<Item> todo;

  /** A loaded region, with the values of its leaves and of its expanded
   *  comparisons */
  struct Region {
    FormulaRegions r;
    std::unordered_map<gate_t, std::string> base;
    std::unordered_set<std::string> stubs;
  };
  /** The children of the wide gate being written, @c batch_from onwards */
  Region batch;
  std::size_t batch_from = 0;
  bool batch_loaded = false;

  /** Thrown by @c CappedFormula past its cap */
  struct TooLarge {};
  /** @c Formula, refusing a value longer than @c cap */
  class CappedFormula : public semiring::Formula {
    std::size_t cap;
    std::string check(std::string v) const {
      if(v.size() > cap)
        throw TooLarge();
      return v;
    }
public:
    explicit CappedFormula(std::size_t cap) : cap(cap) {}
    value_type plus(const std::vector<value_type> &v) const override {
      return check(Formula::plus(v));
    }
    value_type times(const std::vector<value_type> &v) const override {
      return check(Formula::times(v));
    }
    value_type plus_of(const std::vector<const value_type *> &v) const override {
      return check(Formula::plus_of(v));
    }
    value_type times_of(const std::vector<const value_type *> &v) const override {
      return check(Formula::times_of(v));
    }
    value_type monus(value_type x, value_type y) const override {
      return check(Formula::monus(std::move(x), std::move(y)));
    }
  };

  /* The number of contributors to the aggregates the comparison @p g
   * reads */
  static std::size_t contributors(const GenericCircuit &c, gate_t g) {
    std::size_t k = 0;
    std::unordered_set<gate_t> seen;
    std::vector<gate_t> stack(c.getWires(g).begin(), c.getWires(g).end());
    while(!stack.empty()) {
      const gate_t u = stack.back();
      stack.pop_back();
      if(!seen.insert(u).second)
        continue;
      if(c.getGateType(u) == gate_agg)
        k += c.getWires(u).size();
      else
        for(const auto w : c.getWires(u))
          stack.push_back(w);
    }
    return k;
  }

  /* The comparisons on aggregates below @p root written as sr_formula
   * writes them, a sum over the possible worlds of each group, unless that
   * sum could be longer than a row: they are then written as they are, the
   * sum being exponential in the size of the group.  The worlds are
   * enumerated before any of the sum is written, so the expansion is tried
   * only when there are few enough of them; and it is given up, the region
   * restored, as soon as a value of it is longer than a row.  A comparison
   * whose aggregate is a named gate, not in the region, is not expanded. */
  void expand_comparisons(Region &g, gate_t root) {
    GenericCircuit &c = g.r.circuit;
    const auto cmps = provsql_having_detail::collect_sp_cmp_gates(c, root);
    if(cmps.empty())
      return;
    /* A row of max_size symbols, at a few bytes each */
    const std::size_t cap = 8 * SLACK * max_size;
    std::size_t k = 0;
    for(const auto x : cmps)
      k += contributors(c, x);
    if(k >= 8 * sizeof(std::size_t) || (std::size_t(1) << k) > cap
       || (provsql_max_worlds > 0
           && (std::size_t(1) << k) > (std::size_t) provsql_max_worlds))
      return;
    GenericCircuit saved_circuit = c;
    auto saved_base = g.base;
    try {
      provsql_having(c, root, g.base, CappedFormula(cap));
    } catch(const TooLarge &) {
      c = std::move(saved_circuit);
      g.base = std::move(saved_base);
    }
  }

  /* Ask the store for @p q, and read the labels of the leaves */
  void load(Region &g, const FormulaRequest &q) {
    g.r = getFormulaRegions({q}, max_size, SLACK * max_size);
    g.base.clear();
    g.stubs.clear();
    for(const auto &s : g.r.stubs)
      g.stubs.insert(s.uuid);
    const bool drop_table =
      join_with_temp_uuids(table, g.r.circuit.getMappableUUIDs());
    semiring::Formula sr;
    initialize_provenance_mapping<std::string>(constants, g.r.circuit, g.base,
      [&sr](const char *v) { return sr.parse_leaf(v); }, drop_table);
  }

  /* The name of a named gate, given when first met, its row then queued */
  const std::string &name_of(const std::string &uuid) {
    auto it = names.find(uuid);
    if(it == names.end()) {
      it = names.emplace(uuid, "#" + std::to_string(next_name++)).first;
      todo.push_back({uuid, OWN, 0});
    }
    return it->second;
  }

  /* The formula of @p g in its region, the named gates read as names */
  std::string render(Region &reg, gate_t g) {
    GenericCircuit &c = reg.r.circuit;
    semiring::Formula sr;
    const std::string gu = c.getUUID(g);
    if(reg.stubs.count(gu))
      return name_of(gu);
    auto b = reg.base.find(g);
    if(b != reg.base.end())
      return sr.to_text(b->second);
    std::unordered_map<gate_t, std::string> m;
    std::unordered_set<gate_t> seen{g};
    std::vector<gate_t> stack;
    const auto &w = c.getWires(g);
    for(auto it = w.rbegin(); it != w.rend(); ++it)
      stack.push_back(*it);
    while(!stack.empty()) {
      provsql_poll_interrupt();
      const gate_t u = stack.back();
      stack.pop_back();
      if(!seen.insert(u).second)
        continue;
      const std::string uu = c.getUUID(u);
      if(!uu.empty() && reg.stubs.count(uu)) {
        m.emplace(u, name_of(uu));
        continue;
      }
      auto bu = reg.base.find(u);
      if(bu != reg.base.end()) {
        m.emplace(u, bu->second);
        continue;
      }
      const auto &wu = c.getWires(u);
      for(auto it = wu.rbegin(); it != wu.rend(); ++it)
        stack.push_back(*it);
    }
    c.evaluate(g, m, sr);
    return sr.to_text(std::move(m.at(g)));
  }

  /* The row of a wide gate, its children as #k.1 ... #k.n */
  static std::string summary(const FormulaStub &s, const std::string &nm) {
    const auto n = s.nb_children;
    const std::vector<std::string> v{nm + ".1", "…", nm + "." + std::to_string(n)};
    semiring::Formula sr;
    std::string r;
    switch(s.type) {
    case gate_plus:
      r = sr.plus(v);
      break;
    case gate_times:
      r = sr.times(v);
      break;
    default:
      r = sr.agg(getAggregationOperator(s.info1), v);
    }
    return sr.to_text(std::move(r)) + " (" + std::to_string(n) + " terms)";
  }

public:
  FormulaRows(const constants_t &constants, pg_uuid_t token, Oid table,
              std::size_t max_size)
    : constants(constants), table(table), max_size(max_size) {
    name_of(uuid2string(token));
  }

  /** The next row, as (name, formula); false when there is none left. */
  bool next(std::string &name, std::string &formula) {
    if(todo.empty())
      return false;
    Item &item = todo.front();
    if(item.child == OWN) {
      const std::string uuid = item.uuid;
      name = names.at(uuid);
      Region reg;
      load(reg, {string2uuid(uuid), 0, 0});
      if(reg.stubs.count(uuid)) {
        for(const auto &s : reg.r.stubs)
          if(s.uuid == uuid) {
            formula = summary(s, name);
            item.child = 0;
            item.nb = s.nb_children;
          }
        return true;
      }
      todo.pop_front();
      const gate_t g = reg.r.circuit.getGate(uuid);
      expand_comparisons(reg, g);
      formula = render(reg, g);
      return true;
    }

    const std::string uuid = item.uuid;
    const std::size_t i = item.child++;
    const std::size_t nb = item.nb;
    if(!batch_loaded || i >= batch_from + batch.r.children.size()) {
      load(batch, {string2uuid(uuid), i, std::min(BATCH, nb - i)});
      batch_from = i;
      batch_loaded = true;
      for(const auto &ch : batch.r.children)
        if(!batch.stubs.count(ch))
          expand_comparisons(batch, batch.r.circuit.getGate(ch));
    }
    name = names.at(uuid) + "." + std::to_string(i + 1);
    const std::string ch = batch.r.children.at(i - batch_from);
    if(i + 1 == nb) {
      todo.pop_front();
      batch_loaded = false;
    }
    formula = render(batch, batch.r.circuit.getGate(ch));
    return true;
  }
};

void delete_formula_rows(void *arg)
{
  delete static_cast<FormulaRows *>(arg);
}

}

/**
 * @brief The formula of a token as rows (name, formula), each bounded in
 *        size: @c sr_formula with the large sub-terms named.
 */
Datum sr_formula_rows(PG_FUNCTION_ARGS)
{
  FuncCallContext *funcctx;

  if(SRF_IS_FIRSTCALL()) {
    funcctx = SRF_FIRSTCALL_INIT();
    if(PG_ARGISNULL(0))
      SRF_RETURN_DONE(funcctx);
    /* The token is an ANYELEMENT, as for sr_formula, so that an aggregate
     * of a subquery comes as its agg_token, not as its value */
    constants_t constants = get_constants(true);
    const Oid type = get_fn_expr_argtype(fcinfo->flinfo, 0);
    pg_uuid_t token;
    if(type == constants.OID_TYPE_AGG_TOKEN)
      token = *DatumGetUUIDP(DirectFunctionCall1(
        uuid_in, CStringGetDatum(((agg_token *) PG_GETARG_POINTER(0))->tok)));
    else if(type == UUIDOID || type == constants.OID_TYPE_RANDOM_VARIABLE)
      token = *PG_GETARG_UUID_P(0);
    else
      provsql_error("sr_formula_rows: the token must be a uuid, an agg_token "
                    "or a random_variable, not %s", format_type_be(type));
    const Oid table = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
    const int32 max_size = PG_ARGISNULL(2) ? 50 : PG_GETARG_INT32(2);
    if(max_size < 1)
      provsql_error("sr_formula_rows: max_size must be positive");

    /* The descriptor allocated where it lasts for all the rows */
    MemoryContext old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    TupleDesc tupdesc;
    if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      provsql_error("sr_formula_rows must return a record");
    funcctx->tuple_desc = BlessTupleDesc(tupdesc);
    MemoryContextSwitchTo(old);

    FormulaRows *rows = nullptr;
    try {
      provsql_interrupt_scope interrupt_scope;
      rows = new FormulaRows(constants, token, table, max_size);
    } catch(const CircuitRefusal &r) {
      provsql_cancel_if_interrupted();
      provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
    } catch(const std::exception &e) {
      provsql_cancel_if_interrupted();
      provsql_error("sr_formula_rows: %s", e.what());
    } catch(...) {
      provsql_cancel_if_interrupted();
      provsql_error("sr_formula_rows: Unknown exception");
    }
    funcctx->user_fctx = rows;
    MemoryContextCallback *cb = static_cast<MemoryContextCallback *>(
      MemoryContextAlloc(funcctx->multi_call_memory_ctx,
                         sizeof(MemoryContextCallback)));
    cb->func = delete_formula_rows;
    cb->arg = rows;
    MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);
  }

  funcctx = SRF_PERCALL_SETUP();
  FormulaRows *rows = static_cast<FormulaRows *>(funcctx->user_fctx);
  std::string name, formula;
  bool more = false;
  Datum values[2];
  try {
    provsql_interrupt_scope interrupt_scope;
    more = rows->next(name, formula);
    if(more) {
      if(provsql_max_memory > 0)
        provsql_check_memory();
      values[0] = text_datum(name);
      values[1] = text_datum(formula);
    }
  } catch(const CircuitRefusal &r) {
    provsql_cancel_if_interrupted();
    provsql_unsupported(r.scope(), r.tag(), "%s", r.what());
  } catch(const std::exception &e) {
    provsql_cancel_if_interrupted();
    provsql_error("sr_formula_rows: %s", e.what());
  } catch(...) {
    provsql_cancel_if_interrupted();
    provsql_error("sr_formula_rows: Unknown exception");
  }
  if(!more)
    SRF_RETURN_DONE(funcctx);
  bool nulls[2] = {false, false};
  SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(
                    heap_form_tuple(funcctx->tuple_desc, values, nulls)));
}
