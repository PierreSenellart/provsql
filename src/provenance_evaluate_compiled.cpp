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

#include "provenance_evaluate_compiled.hpp"

#include "semiring/Boolean.h"
#include "semiring/Counting.h"
#include "semiring/Formula.h"
#include "semiring/Why.h"
#include "subset.hpp"
#include "regex"
const char *drop_temp_table = "DROP TABLE IF EXISTS tmp_uuids;";

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
    return *v=='t';
  }, drop_table);

  if(semiring=="boolean") {
    auto val = c.evaluate<semiring::Boolean>(g, provenance_mapping);
    PG_RETURN_BOOL(val);
  } else
    throw CircuitException("Unknown semiring for type varchar: "+semiring);
}
static Datum pec_why(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,
  const std::set<gate_t> &inputs,
  const std::string &semiring,
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
      elog(ERROR, "Complex Why-semiring values for input tuples not currently supported.");
    single.insert(std::string(v));
    result.insert(std::move(single));
    return result;
  },
    drop_table
    );

  if (semiring == "why") {
    auto prov = c.evaluate<semiring::Why>(g, provenance_mapping);

    // Serialize nested set structure: {{x},{y}}
    std::ostringstream oss;
    oss << "{";
    bool firstOuter = true;
    for (const auto& inner : prov) {
      if (!firstOuter) oss << ",";
      firstOuter = false;
      oss << "{";
      bool firstInner = true;
      for (const auto& label : inner) {
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
  } else {
    throw CircuitException("Unknown semiring for type varchar: " + semiring);
  }
}
static Datum pec_varchar(
  const constants_t &constants,
  GenericCircuit &c,
  gate_t g,                       // gate corresponding to the particular row (output tuple) in output
  const std::set<gate_t> &inputs,//base tupe annotations
  const std::string &semiring,
  bool drop_table)
{
//will try to document from here on :"

/*create mapping from input gates( provenance uuids) to formula semiring labels(strings)
 * SPI query that was called before this method produced rows like (value, uuid)
 * where: value= label to show for the inout tuple annotations
 *        uuid= uuid identifying the input gate in the circuit given by generiuc circuit
 *so, formula_mapping[input_gate]=label
 */
  std::unordered_map<gate_t, std::string> formula_mapping;//gate to formula semiring label mapping
  //loop over all SPI  result rows produced by join_with_temp_uuids 
  for (uint64 i = 0; i < SPI_processed; i++) //SPI_provessed = no of rows returned by the SPI query
  {
    HeapTuple tuple = SPI_tuptable->vals[i];//get ith row from SPI result
    TupleDesc tupdesc = SPI_tuptable->tupdesc;// get schema
    char *val  = SPI_getvalue(tuple, tupdesc, 1);//column 1 is "value" of the uuid ,i.e. a formula semiring string label
    char *uuid = SPI_getvalue(tuple, tupdesc, 2);//column 2 is uuid
    gate_t gate = c.getGate(uuid);//uuid to gate id
    formula_mapping[gate] = std::string(val);//gate to formula semiring value
    pfree(val);
    pfree(uuid);
  }
//if join_with_temp_uuids created tmp_uuids ill drop i//if join_with_temp_uuids created tmp_uuids ill drop it//if join_with_temp_uuids created tmp_uuids ill drop it, basically handling the many uuids case
  if (drop_table)
    SPI_exec(drop_temp_table, 0);
  SPI_finish();

  if (semiring != "formula")
    throw CircuitException("Unknown semiring fot type varchar: " + semiring);

  //helper_1: integer parsing from strings
  //"1"=ok "1.0" =not ok "1 "=not okay and so on
  //this would parse numeric constants stored as strings in getExtra(gate)
  auto parse_int_strict = [&](const std::string &s, bool &ok) -> int {
    ok = false;
    if (s.empty()) return 0;//empty string is not a valid integer
    size_t idx = 0;
    try {
      int v = std::stoi(s, &idx);
      if (idx != s.size()) return 0;
      ok = true;
      return v;
    } catch (...) {
      return 0;
    }
  };
//helper_2: repeatedly strip gate_project and gate_eq by replacing it with its first cjild

  auto unwrap_ignored = [&](gate_t x) -> gate_t {
    while (true) {
      gate_type t = c.getGateType(x);
      if (t == gate_project || t == gate_eq) {
        const auto &w = c.getWires(x);
        if (w.empty()) break;
        x = w[0];
        continue;
      }
      break;
    }
    return x;
  };
//helper_3: convert operator in gate_cmp to enum ComparisonOp
  auto map_cmp_op = [&](gate_t cmp_gate, bool &ok) -> ComparisonOp {
    ok = false;
    auto infos = c.getInfos(cmp_gate);
    char *opname = get_opname(infos.first);//OID of postgres operator like >,>=,<,<=....
    if (!opname) return ComparisonOp::EQ;

    std::string s(opname);
    ok = true;
    if (s == "=")  return ComparisonOp::EQ;
    if (s == "<>") return ComparisonOp::NEQ;
    if (s == "<")  return ComparisonOp::LT;
    if (s == "<=") return ComparisonOp::LE;
    if (s == ">")  return ComparisonOp::GT;
    if (s == ">=") return ComparisonOp::GE;

    ok = false;
    return ComparisonOp::EQ;
  };

  //helper_4: This  tries to interpret a semimod gate as (Monoid value, K semiring anotation)
  // regardless of wire order.
  auto semimod_extract_M_and_K = [&](gate_t semimod_gate, int &m_out, gate_t &k_gate_out) -> bool {
    semimod_gate = unwrap_ignored(semimod_gate);
    if (c.getGateType(semimod_gate) != gate_semimod) return false;

    const auto &w = c.getWires(semimod_gate);
    if (w.size() != 2) return false;

    gate_t a = unwrap_ignored(w[0]);
    gate_t b = unwrap_ignored(w[1]);

    // case 1: a is numeric value
    if (c.getGateType(a) == gate_value) {
      bool ok = false;
      int v = parse_int_strict(c.getExtra(a), ok);
      if (!ok) return false;
      m_out = v;
      k_gate_out = b;
      return true;
    }

    // case 2: b is numeric value
    if (c.getGateType(b) == gate_value) {
      bool ok = false;
      int v = parse_int_strict(c.getExtra(b), ok);
      if (!ok) return false;
      m_out = v;
      k_gate_out = a;
      return true;
    }

    return false;
  };

  //helper_5: Accept both constant encodings:
  // - gate_value("C")
  // - gate_semimod(C, gate_one)
  // - gate_semimod(gate_one, C)   
  auto extract_constant_C = [&](gate_t x, int &C_out) -> bool {
    x = unwrap_ignored(x);

    if (c.getGateType(x) == gate_value) {
      bool ok = false;
      int v = parse_int_strict(c.getExtra(x), ok);
      if (!ok) return false;
      C_out = v;
      return true;
    }

    if (c.getGateType(x) == gate_semimod) {
      const auto &w = c.getWires(x);
      if (w.size() != 2) return false;

      gate_t a = unwrap_ignored(w[0]);
      gate_t b = unwrap_ignored(w[1]);

      // (value, one)
      if (c.getGateType(a) == gate_value && c.getGateType(b) == gate_one) {
        bool ok = false;
        int v = parse_int_strict(c.getExtra(a), ok);
        if (!ok) return false;
        C_out = v;
        return true;
      }

      // (one, value)  
      if (c.getGateType(a) == gate_one && c.getGateType(b) == gate_value) {
        bool ok = false;
        int v = parse_int_strict(c.getExtra(b), ok);
        if (!ok) return false;
        C_out = v;
        return true;
      }
    }

    return false;
  };

  //helper_6: to extract tuples (K annotation, M value int) from an agg gate whose children are semimod(...)
  auto extract_tuples_from_agg = [&](gate_t agg_gate,
                                    std::vector<std::string> &labels_out,
                                    std::vector<int> &values_out) -> bool
  {
    agg_gate = unwrap_ignored(agg_gate);
    if (c.getGateType(agg_gate) != gate_agg) return false;

    const auto &children = c.getWires(agg_gate);

    labels_out.clear();
    values_out.clear();
    labels_out.reserve(children.size());
    values_out.reserve(children.size());

    for (gate_t ch : children) {
      ch = unwrap_ignored(ch);
      if (c.getGateType(ch) != gate_semimod) return false;

      int m = 0;//monoid value 
      gate_t k_gate{};//provenance expression 
      if (!semimod_extract_M_and_K(ch, m, k_gate)) return false;

      //instantiate the K gate on formula semiring to get formula semiring expression 
      std::string k_str = c.evaluate<semiring::Formula>(k_gate, formula_mapping);

      labels_out.push_back(std::move(k_str));//store label string
      values_out.push_back(m);//store numeric value
    }

    return true;
  };

  //helper_7 for flipping comparison operator to support agg op cmp and cmp op agg both.
  auto flip_op =[&](ComparisonOp op)-> ComparisonOp {
    switch (op) {
      case ComparisonOp::LT: return ComparisonOp::GT;
      case ComparisonOp::LE: return ComparisonOp::GE;    
      case ComparisonOp::GT: return ComparisonOp::LT;    
      case ComparisonOp::GT: return ComparisonOp::LE;
      case ComparisonOp::EQ: return ComparisonOp::EQ;
      case ComparisonOp::NEQ: return ComparisonOp::NEQ;    
    }
    return op;
  };



  // find the HAVING rewrite pattern: times(delta(.), cmp(....)) possibly wrapped
  gate_t root = unwrap_ignored(g);

  gate_t cmp_gate{};
  bool found = false;

  if (c.getGateType(root) == gate_times) {
    const auto &w = c.getWires(root);
    if (w.size() == 2) {
      gate_t a = unwrap_ignored(w[0]);
      gate_t b = unwrap_ignored(w[1]);

      if (c.getGateType(a) == gate_cmp && c.getGateType(b) == gate_delta) {
        cmp_gate = a; found = true;
      } else if (c.getGateType(b) == gate_cmp && c.getGateType(a) == gate_delta) {
        cmp_gate = b; found = true;
      }
    }
  }

  if (found) {
    const auto &cw = c.getWires(cmp_gate);
    if (cw.size() == 2) {
      gate_t L = unwrap_ignored(cw[0]);
      gate_t R = unwrap_ignored(cw[1]);

      bool okop = false;
      ComparisonOp op = map_cmp_op(cmp_gate, okop);

      if (okop) {
       //case1: agg op cmp 
        if (c.getGateType(L) == gate_agg) {
          int C = 0;
          if (extract_constant_C(R, C)) {
            std::vector<std::string> labels;
            std::vector<int> values;
            if (extract_tuples_from_agg(L, labels, values))
            {
              std::vector<uint64_t> worlds = enumerate_valid_worlds(values, C, op);

              // build formula with ⊕ ⊗ ⊖ here
              if (worlds.empty()) {
                std::string out = "𝟘";
                text *result = (text *) palloc(VARHDRSZ + out.size());
                SET_VARSIZE(result, VARHDRSZ + out.size());
                memcpy(VARDATA(result), out.c_str(), out.size());
                PG_RETURN_TEXT_P(result);
              }

              std::vector<std::string> disjuncts;//store each world W
              disjuncts.reserve(worlds.size());//reserve no. of worlds

              auto join = [&](const std::vector<std::string> &v, const char *sep) -> std::string {
                if (v.empty()) return "";
                std::ostringstream oss;
                oss << v[0];
                for (size_t i = 1; i < v.size(); ++i) oss << sep << v[i];
                return oss.str();
              };

              const size_t n = labels.size();
              for (uint64_t mask : worlds) {
                std::vector<std::string> present;
                std::vector<std::string> missing;
                present.reserve(n);
                missing.reserve(n);

                for (size_t i = 0; i < n; ++i) {
                  if (mask & (1ULL << i)) present.push_back(labels[i]);
                  else                    missing.push_back(labels[i]);
                }

                std::string present_expr;
                if (present.empty()) present_expr = "𝟙";
                else if (present.size() == 1) present_expr = present[0];
                else present_expr = "(" + join(present, " ⊗ ") + ")";

                std::string term;
                if (!missing.empty()) {
                  std::string missing_expr;
                  if (missing.size() == 1) missing_expr = missing[0];
                  else missing_expr = "(" + join(missing, " ⊕ ") + ")";

                  term = "(" + present_expr + " ⊗ (𝟙 ⊖ " + missing_expr + "))";
                } else {
                  term = present_expr;
                }

                disjuncts.push_back(std::move(term));
              }

              std::string out;
              if (disjuncts.size() == 1) out = disjuncts[0];
              else out = "(" + join(disjuncts, " ⊕ ") + ")";

              text *result = (text *) palloc(VARHDRSZ + out.size());
              SET_VARSIZE(result, VARHDRSZ + out.size());
              memcpy(VARDATA(result), out.c_str(), out.size());
              PG_RETURN_TEXT_P(result);
            }
          }
        }
        //case2: cmp op agg

        if (c.getGateType(R) == gate_agg) {
          int C = 0;
          if (extract_constant_C(L,C)){
            std::vector<std::string> labels;
            std::vector<int> values;

            if (extract_tuples_from_agg(R, labels , values))
            {
             std::vector<uint64_t> worlds = enumerate_valid_worlds(values, C, op);
             
             if (worlds.empty()){
               std::string out = "𝟘";
                text *result = (text *) palloc(VARHDRSZ + out.size());
                SET_VARSIZE(result, VARHDRSZ + out.size());
                memcpy(VARDATA(result), out.c_str(), out.size());
                PG_RETURN_TEXT_P(result);

             }
             std::vector<std::string> disjuncts;
             disjuncts.reserve(worlds.size());

             auto join = [&](const std::vector<std::string> &v, const char *sep) -> std::string {
                if (v.empty()) return "";
                std::ostringstream oss;
                oss << v[0];
                for (size_t i = 1; i < v.size(); ++i) oss << sep << v[i];
                return oss.str();
              }; 

              const size_t n = labels.size();
              for (uint64_t mask : worlds) {
                std::vector<std::string> present;
                std::vector<std::string> missing;
                present.reserve(n);
                missing.reserve(n);

                for (size_t i = 0; i < n; ++i) {
                  if (mask & (1ULL << i)) present.push_back(labels[i]);
                  else                    missing.push_back(labels[i]);
                }

                std::string present_expr;
                if (present.empty()) present_expr = "𝟙";
                else if (present.size() == 1) present_expr = present[0];
                else present_expr = "(" + join(present, " ⊗ ") + ")";

                std::string term;
                if (!missing.empty()) {
                  std::string missing_expr;
                  if (missing.size() == 1) missing_expr = missing[0];
                  else missing_expr = "(" + join(missing, " ⊕ ") + ")";

                  term = "(" + present_expr + " ⊗ (𝟙 ⊖ " + missing_expr + "))";
                } else {
                  term = present_expr;
                }

                disjuncts.push_back(std::move(term));
              }

              std::string out;
              if (disjuncts.size() == 1) out = disjuncts[0];
              else out ="("+join(disjuncts, " ⊕ ") + ")";

              text *result = (text *) palloc(VARHDRSZ) +out.size();
              SET_VARSIZE(result, VARHDRSZ + out.size());
              memcpy(VARDATA(result), out.c_str(), out.size());
              PG_RETURN_TEXT_P(result);
            }
          }
        }
      }
    }
  }

  // fallback if there is no HAVING
  std::string s = c.evaluate<semiring::Formula>(g, formula_mapping);

  text *result = (text *) palloc(VARHDRSZ + s.size());
  SET_VARSIZE(result, VARHDRSZ + s.size());
  memcpy(VARDATA(result), s.c_str(), s.size());
  PG_RETURN_TEXT_P(result);
}
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

  if(semiring=="counting") {
    auto val = c.evaluate<semiring::Counting>(g, provenance_mapping);
    PG_RETURN_INT32(val);
  } else
    throw CircuitException("Unknown semiring for type int: "+semiring);
}

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
  bool drop_table = join_with_temp_uuids(table, inputs_uuid);

  constants_t constants = get_constants(true);

  if (type == constants.OID_TYPE_VARCHAR)
  {
    if (semiring == "why")
      return pec_why(constants, c, g, inputs, semiring, drop_table);
    else
      return pec_varchar(constants, c, g, inputs, semiring, drop_table);
  }
  else if(type==constants.OID_TYPE_INT)
    return pec_int(constants, c, g, inputs, semiring, drop_table);
  else if(type==constants.OID_TYPE_BOOL)
    return pec_bool(constants, c, g, inputs, semiring, drop_table);
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
