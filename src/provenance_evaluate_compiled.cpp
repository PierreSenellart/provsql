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
 * The function first builds a provenance mapping (input-gate UUID → semiring
 * value) by querying the @c tmp_uuids table via SPI
 * (using @c initialize_provenance_mapping()), then evaluates the
 * @c GenericCircuit with @c GenericCircuit::evaluate() and returns the
 * result as text.
 */
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

/**
 * @brief Evaluate the Boolean semiring provenance for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate of the sub-circuit.
 * @param inputs      Set of input gate IDs.
 * @param semiring    Semiring name (must be "boolean").
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Bool datum with the evaluated provenance.
 */
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
    return *v != 'f' && *v != '0';
  }, drop_table);

  if(semiring!="boolean")
    throw CircuitException("Unknown semiring for type varchar: "+semiring);

  provsql_try_having_boolean(c,g,provenance_mapping);
  bool out = c.evaluate<semiring::Boolean>(g, provenance_mapping, semiring::Boolean());

  PG_RETURN_BOOL(out);
}

/**
 * @brief Evaluate the Boolean-expression semiring provenance for a circuit.
 * @param constants  Extension OID cache.
 * @param bc         Boolean circuit to render as a formula.
 * @param root       Root gate of the circuit.
 * @return           Text datum with the formula string.
 */
static Datum pec_boolexpr(
  const constants_t &constants,
  BooleanCircuit &bc,
  gate_t root)
{
  std::string out = bc.toString(root);

  text *result = (text *) palloc(VARHDRSZ + out.size());
  SET_VARSIZE(result, VARHDRSZ + out.size());
  memcpy(VARDATA(result), out.c_str(), out.size());
  PG_RETURN_TEXT_P(result);
}

/**
 * @brief Evaluate the Why-provenance semiring for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Varchar datum containing the serialised Why-provenance.
 */
static Datum pec_why(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
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
      provsql_error("Complex Why-semiring values for input tuples not currently supported.");
    single.insert(std::string(v));
    result.insert(std::move(single));
    return result;
  },
    drop_table
    );

  provsql_try_having_why(c, g, provenance_mapping);
  semiring::why_provenance_t prov = c.evaluate<semiring::Why>(g, provenance_mapping, semiring::Why());

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

/**
 * @brief Evaluate the How-provenance semiring (canonical polynomial provenance) for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Varchar datum containing the canonical sum-of-products
 *                    representation of the polynomial.
 */
static Datum pec_how(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  bool drop_table)
{
  std::unordered_map<gate_t, semiring::how_provenance_t> provenance_mapping;

  initialize_provenance_mapping<semiring::how_provenance_t>(
    constants,
    c,
    provenance_mapping,
    [](const char *v) {
    if(strchr(v, '{') || strchr(v, '+') || strchr(v, '*') || strchr(v, '^'))
      provsql_error("Complex How-semiring values for input tuples not currently supported.");
    semiring::how_monomial_t mono;
    mono[std::string(v)] = 1u;
    return semiring::how_provenance_t{ { std::move(mono), 1u } };
  },
    drop_table
    );

  provsql_try_having_how(c, g, provenance_mapping);
  semiring::how_provenance_t prov =
    c.evaluate<semiring::How>(g, provenance_mapping, semiring::How());

  // Canonical sum-of-products serialisation: monomials in lexicographic
  // order (std::map iteration order), variables within a monomial also
  // lexicographic. Multiplication is the dot operator U+22C5; exponents
  // are ASCII "^k". e.g. "2⋅Alice⋅Bob^2 + 3⋅Charlie", "0", "1".
  std::ostringstream oss;
  if (prov.empty()) {
    oss << "0";
  } else {
    bool firstMono = true;
    for (const auto &[mono, coeff] : prov) {
      if (!firstMono) oss << " + ";
      firstMono = false;
      bool need_dot = false;
      if (coeff != 1 || mono.empty()) {
        oss << coeff;
        need_dot = true;
      }
      for (const auto &[var, exp] : mono) {
        if (need_dot) oss << "⋅";
        need_dot = true;
        oss << var;
        if (exp != 1) oss << "^" << exp;
      }
    }
  }

  std::string out = oss.str();
  text *result = (text *) palloc(VARHDRSZ + out.size());
  SET_VARSIZE(result, VARHDRSZ + out.size());
  memcpy(VARDATA(result), out.c_str(), out.size());
  PG_RETURN_TEXT_P(result);
}

/**
 * @brief Evaluate the Which-provenance (lineage) semiring for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Varchar datum containing the serialised Which-provenance.
 */
static Datum pec_which(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  bool drop_table)
{
  std::unordered_map<gate_t, semiring::which_provenance_t> provenance_mapping;

  initialize_provenance_mapping<semiring::which_provenance_t>(
    constants,
    c,
    provenance_mapping,
    [](const char *v) {
    if(strchr(v, '{'))
      provsql_error("Complex Which-semiring values for input tuples not currently supported.");
    std::set<std::string> single;
    single.insert(std::string(v));
    return semiring::which_provenance_t(std::move(single));
  },
    drop_table
    );

  provsql_try_having_which(c, g, provenance_mapping);
  semiring::which_provenance_t prov =
    c.evaluate<semiring::Which>(g, provenance_mapping, semiring::Which());

  std::ostringstream oss;
  if(!prov.has_value()) {
    oss << "⊥";
  } else {
    oss << "{";
    bool first = true;
    for (const auto &label : *prov) {
      if (!first) oss << ",";
      first = false;
      oss << label;
    }
    oss << "}";
  }

  std::string out = oss.str();
  text *result = (text *) palloc(VARHDRSZ + out.size());
  SET_VARSIZE(result, VARHDRSZ + out.size());
  memcpy(VARDATA(result), out.c_str(), out.size());
  PG_RETURN_TEXT_P(result);
}

/**
 * @brief Evaluate a varchar semiring provenance for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param semiring    Semiring name (e.g. "formula").
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Varchar datum containing the serialised provenance.
 */
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
    [](const char *v) {
    return std::string(v);
  },
    drop_table
    );

  if (semiring!= "formula")
    throw CircuitException("Unknown seimring for type varchar: " + semiring);

  provsql_try_having_formula(c, g, provenance_mapping);
  std::string s = c.evaluate<semiring::Formula>(g, provenance_mapping, semiring::Formula());

  text *result = (text *) palloc(VARHDRSZ + s.size());
  SET_VARSIZE(result, VARHDRSZ + s.size());
  memcpy(VARDATA(result), s.c_str(), s.size());
  PG_RETURN_TEXT_P(result);

}
#if PG_VERSION_NUM >= 140000
/**
 * @brief Evaluate the interval-union semiring provenance over a
 *        multirange carrier.
 * @param constants        Extension OID cache.
 * @param c                Generic circuit to evaluate.
 * @param g                Root gate.
 * @param inputs           Set of input gate IDs.
 * @param multirange_oid   OID of the multirange type used for parsing
 *                         leaf values and constructing zero/one.
 * @param drop_table       Whether the temporary UUID table should be dropped.
 * @return                 Multirange Datum with the evaluated provenance.
 */
static Datum pec_multirange(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  Oid multirange_oid,
  bool drop_table)
{
  std::unordered_map<gate_t, Datum> provenance_mapping;
  semiring::IntervalUnion sr(multirange_oid);
  initialize_provenance_mapping<Datum>(constants, c, provenance_mapping, [&sr](const char *v) {
    return sr.parse(v);
  }, drop_table);

  provsql_try_having_multirange(c, g, provenance_mapping, sr);
  Datum out = c.evaluate<semiring::IntervalUnion>(g, provenance_mapping, sr);

  PG_RETURN_DATUM(out);
}
#endif

/**
 * @brief Evaluate the min-max / max-min m-semiring provenance over a
 *        user-defined PostgreSQL enum carrier.
 * @param constants    Extension OID cache.
 * @param c            Generic circuit to evaluate.
 * @param g            Root gate.
 * @param inputs       Set of input gate IDs.
 * @param enum_oid     OID of the carrier enum type used for parsing
 *                     leaf values and resolving bottom/top.
 * @param reverse      @c false for @c sr_minmax (security shape:
 *                     @f$\oplus = \min, \otimes = \max@f$),
 *                     @c true for @c sr_maxmin (fuzzy shape).
 * @param drop_table   Whether the temporary UUID table should be dropped.
 * @return             Enum Datum (an Oid) with the evaluated provenance.
 */
static Datum pec_anyenum(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  Oid enum_oid,
  bool reverse,
  bool drop_table)
{
  std::unordered_map<gate_t, Datum> provenance_mapping;
  semiring::MinMax sr(enum_oid, reverse);
  initialize_provenance_mapping<Datum>(constants, c, provenance_mapping, [&sr](const char *v) {
    return sr.parse(v);
  }, drop_table);

  provsql_try_having_minmax(c, g, provenance_mapping, sr);
  Datum out = c.evaluate<semiring::MinMax>(g, provenance_mapping, sr);

  PG_RETURN_DATUM(out);
}

/**
 * @brief Evaluate a float semiring provenance for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param semiring    Semiring name ("tropical", "viterbi" or "lukasiewicz").
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Float8 datum with the evaluated provenance.
 */
static Datum pec_float(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
  bool drop_table)
{
  std::unordered_map<gate_t, double> provenance_mapping;
  initialize_provenance_mapping<double>(constants, c, provenance_mapping, [](const char *v) {
    return atof(v);
  }, drop_table);

  double out;
  if(semiring=="tropical") {
    provsql_try_having_tropical(c, g, provenance_mapping);
    out = c.evaluate<semiring::Tropical>(g, provenance_mapping, semiring::Tropical());
  } else if(semiring=="viterbi") {
    provsql_try_having_viterbi(c, g, provenance_mapping);
    out = c.evaluate<semiring::Viterbi>(g, provenance_mapping, semiring::Viterbi());
  } else if(semiring=="lukasiewicz") {
    provsql_try_having_lukasiewicz(c, g, provenance_mapping);
    out = c.evaluate<semiring::Lukasiewicz>(g, provenance_mapping, semiring::Lukasiewicz());
  } else
    throw CircuitException("Unknown semiring for type float: "+semiring);

  PG_RETURN_FLOAT8(out);
}

/**
 * @brief Evaluate an integer semiring provenance for a circuit.
 * @param constants   Extension OID cache.
 * @param c           Generic circuit to evaluate.
 * @param g           Root gate.
 * @param inputs      Set of input gate IDs.
 * @param semiring    Semiring name (e.g. "counting").
 * @param drop_table  Whether the temporary UUID table should be dropped.
 * @return            Int32 datum with the evaluated provenance.
 */
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

  provsql_try_having_counting(c, g, provenance_mapping);
  unsigned out = c.evaluate<semiring::Counting>(g, provenance_mapping, semiring::Counting());

  PG_RETURN_INT32((int32) out);

}

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

  if(semiring=="boolexpr") {
    gate_t root;
    BooleanCircuit bc = getBooleanCircuit(token, root);
    return pec_boolexpr(constants, bc, root);
  }

  GenericCircuit c = getGenericCircuit(token);
  auto g = c.getGate(uuid2string(token));
  auto inputs = c.getInputs();

  std::vector<std::string> inputs_uuid;
  std::transform(inputs.begin(), inputs.end(), std::back_inserter(inputs_uuid), [&c](auto x) {
    return c.getUUID(x);
  });
  bool drop_table = join_with_temp_uuids(table, inputs_uuid);

  if (type == constants.OID_TYPE_VARCHAR)
  {
    if (semiring == "why")
      return pec_why(constants, c, g, inputs, drop_table);
    else if (semiring == "how")
      return pec_how(constants, c, g, inputs, drop_table);
    else if (semiring == "which")
      return pec_which(constants, c, g, inputs, drop_table);
    else
      return pec_varchar(constants, c, g, inputs, semiring, drop_table);
  }
  else if(type==constants.OID_TYPE_INT)
    return pec_int(constants, c, g, inputs, semiring, drop_table);
  else if(type==constants.OID_TYPE_FLOAT)
    return pec_float(constants, c, g, inputs, semiring, drop_table);
  else if(type==constants.OID_TYPE_BOOL)
    return pec_bool(constants, c, g, inputs, semiring, drop_table);
#if PG_VERSION_NUM >= 140000
  else if(type==constants.OID_TYPE_TSTZMULTIRANGE) {
    if(semiring!="temporal" && semiring!="interval_union")
      throw CircuitException("Unknown semiring for type tstzmultirange: "+semiring);
    return pec_multirange(constants, c, g, inputs, constants.OID_TYPE_TSTZMULTIRANGE, drop_table);
  }
  else if(type==constants.OID_TYPE_NUMMULTIRANGE) {
    if(semiring!="interval_union")
      throw CircuitException("Unknown semiring for type nummultirange: "+semiring);
    return pec_multirange(constants, c, g, inputs, constants.OID_TYPE_NUMMULTIRANGE, drop_table);
  }
  else if(type==constants.OID_TYPE_INT4MULTIRANGE) {
    if(semiring!="interval_union")
      throw CircuitException("Unknown semiring for type int4multirange: "+semiring);
    return pec_multirange(constants, c, g, inputs, constants.OID_TYPE_INT4MULTIRANGE, drop_table);
  }
#endif
  else if(get_typtype(type) == TYPTYPE_ENUM) {
    if(semiring=="minmax")
      return pec_anyenum(constants, c, g, inputs, type, false, drop_table);
    else if(semiring=="maxmin")
      return pec_anyenum(constants, c, g, inputs, type, true, drop_table);
    else
      throw CircuitException("Unknown semiring for enum type: "+semiring);
  }
  else
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
