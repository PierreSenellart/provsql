/**
 * @file having_semantics.cpp
 * @brief Helper definitions for HAVING-clause provenance evaluation.
 *
 * Defines the small non-template helpers declared in
 * @c provsql_having_detail in @c having_semantics.hpp.  The actual
 * possible-worlds logic (enumeration and the MIN / MAX closed form) is
 * the @c provsql_having() template in the header.
 */
extern "C" {
#include "postgres.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "fmgr.h"
}
#include "c_cpp_compatibility.h"

#include <algorithm>
#include <climits>
#include <strings.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "having_semantics.hpp"

namespace provsql_having_detail {

// The comparison-domain type of a HAVING aggregate is the aggregate's
// result type, stored in info2 of the gate_agg (set by
// provenance_aggregate as set_infos(agg, aggfnoid, aggtype)).  This
// classifies it so the cmp can be evaluated in the right domain.
bool aggtype_is_text(unsigned oid) {
  switch (oid) {
    case TEXTOID: case VARCHAROID: case BPCHAROID: case CHAROID: case NAMEOID:
      return true;
    default:
      return false;
  }
}

bool aggtype_is_integer(unsigned oid) {
  switch (oid) {
    case INT2OID: case INT4OID: case INT8OID:
      return true;
    default:
      return false;
  }
}

bool aggtype_is_boolean(unsigned oid) {
  return oid == BOOLOID;
}

// array_agg's result type is the array type (e.g. boolean[]); its element type
// is the comparison domain.  PostgreSQL serialises a scalar bool as
// 'true'/'false' but a bool array element as 't'/'f', so the array_agg
// comparison must know when the elements are boolean to reconcile the two.
bool aggtype_elem_is_boolean(unsigned oid) {
  if (oid == BOOLOID)
    return true;
  Oid elem = get_element_type(oid);
  return elem == BOOLOID;
}

// Types handled by the numeric comparison domain (scaled to a common integer
// grid).  choose() over these is evaluated there -- including ordering
// comparisons; choose() over any other type falls to the value-as-text domain.
bool aggtype_is_numeric(unsigned oid) {
  switch (oid) {
    case INT2OID: case INT4OID: case INT8OID:
    case FLOAT4OID: case FLOAT8OID: case NUMERICOID:
      return true;
    default:
      return false;
  }
}

// Dense ranks of the values and of the threshold under the comparison
// function of their type (a date, a timestamp...), read by the type's input
// function from the text the gates hold.
bool rank_values_by_type(unsigned typoid, const std::vector<std::string> &vals,
                         const std::string &threshold,
                         std::vector<long> &ranks, long &threshold_rank) {
  TypeCacheEntry *tce = lookup_type_cache(typoid, TYPECACHE_CMP_PROC_FINFO);
  if (tce == NULL || !OidIsValid(tce->cmp_proc))
    return false;
  Oid typinput, typioparam;
  getTypeInputInfo(typoid, &typinput, &typioparam);
  Oid collation = get_typcollation(typoid);

  std::vector<Datum> datums;
  datums.reserve(vals.size() + 1);
  for (const auto &v : vals)
    datums.push_back(OidInputFunctionCall(typinput, const_cast<char *>(v.c_str()),
                                          typioparam, -1));
  datums.push_back(OidInputFunctionCall(typinput,
                                        const_cast<char *>(threshold.c_str()),
                                        typioparam, -1));

  auto cmp = [&](size_t a, size_t b) -> int {
    return DatumGetInt32(FunctionCall2Coll(&tce->cmp_proc_finfo, collation,
                                           datums[a], datums[b]));
  };
  std::vector<size_t> order(datums.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return cmp(a, b) < 0; });

  std::vector<long> rank(datums.size());
  long r = 0;
  for (size_t k = 0; k < order.size(); ++k) {
    if (k > 0 && cmp(order[k - 1], order[k]) != 0)
      ++r;
    rank[order[k]] = r;
  }
  ranks.assign(rank.begin(), rank.end() - 1);
  threshold_rank = rank.back();
  return true;
}

// Parse a PostgreSQL array output literal -- "{1,2}", "{a,\"b,c\"}" -- into its
// top-level element texts (surrounding double quotes removed, backslash escapes
// resolved; a NULL element becomes array_null_element()).  Returns false on a malformed or nested-array literal.  Sufficient
// for one-dimensional arrays of scalar elements, which is what array_agg over a
// provenance-tracked column produces.
bool parse_array_literal(const std::string &s, std::vector<std::string> &out) {
  out.clear();
  size_t i = 0, n = s.size();
  while (i < n && isspace((unsigned char) s[i])) i++;
  if (i >= n || s[i] != '{') return false;
  i++;
  while (i < n && isspace((unsigned char) s[i])) i++;
  if (i < n && s[i] == '}') return true;   // empty array
  while (i < n) {
    std::string elem;
    while (i < n && isspace((unsigned char) s[i])) i++;
    if (i < n && s[i] == '"') {
      i++;
      while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n) { elem.push_back(s[i + 1]); i += 2; }
        else { elem.push_back(s[i]); i++; }
      }
      if (i >= n) return false;
      i++;   // closing quote
    } else {
      while (i < n && s[i] != ',' && s[i] != '}') { elem.push_back(s[i]); i++; }
      while (!elem.empty() && isspace((unsigned char) elem.back())) elem.pop_back();
      // An unquoted NULL (any case) is the NULL element; the string 'NULL' is
      // always output quoted.
      if (elem.size() == 4 && strcasecmp(elem.c_str(), "NULL") == 0)
        elem = array_null_element();
    }
    out.push_back(elem);
    while (i < n && isspace((unsigned char) s[i])) i++;
    if (i < n && s[i] == ',') { i++; continue; }
    if (i < n && s[i] == '}') return true;
    return false;
  }
  return false;
}

// Parse a plain decimal literal ("-12.340", "6", "6.5") into a scaled
// integer: the value is @c mantissa * 10^(-scale).  Returns false on
// exponential notation, inf / nan, or anything that is not a plain
// decimal -- those fall back to the existing (string / error) handling.
bool parse_decimal_scaled(const std::string &s, long &mantissa, int &scale) {
  if (s.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
  std::string digits;
  int sc = 0;
  bool seen_dot = false, seen_digit = false;
  for (; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '.') {
      if (seen_dot) return false;
      seen_dot = true;
    } else if (ch >= '0' && ch <= '9') {
      digits.push_back(ch);
      if (seen_dot) ++sc;
      seen_digit = true;
    } else {
      return false;                 // 'e'/'E', inf, nan, separators, ...
    }
  }
  if (!seen_digit) return false;
  // Drop trailing zeros in the fractional part: they do not change the value
  // (15.0000000000000000 == 15, 15.50 == 15.5) but inflate the scale, and a
  // large scale forces every value to be rescaled to a huge integer grid that
  // the value-aware sum DP cannot represent.  Numeric division in particular
  // yields such trailing-zero-padded thresholds.  Only fractional zeros are
  // dropped (scale > 0); trailing zeros of an integer (100) are significant.
  while (sc > 0 && !digits.empty() && digits.back() == '0') {
    digits.pop_back();
    --sc;
  }
  try {
    std::size_t pos = 0;
    long long val = std::stoll(digits, &pos);
    if (pos != digits.size()) return false;
    mantissa = neg ? -static_cast<long>(val) : static_cast<long>(val);
    scale = sc;
    return true;
  } catch (...) {                    // out_of_range / invalid
    return false;
  }
}

// Rescale a (mantissa, scale) decimal to a common target scale, i.e.
// mantissa * 10^(target_scale - scale).  Returns false on overflow.
bool rescale_to(long mantissa, int scale, int target_scale, long &out) {
  long factor = 1;
  for (int k = 0; k < target_scale - scale; ++k) {
    if (factor > (LONG_MAX / 10)) return false;
    factor *= 10;
  }
  if (mantissa != 0 &&
      (mantissa > LONG_MAX / factor || mantissa < LONG_MIN / factor))
    return false;
  out = mantissa * factor;
  return true;
}

// Map a cmp gate's Postgres operator to subset.cpp's ComparisonOperator
ComparisonOperator map_cmp_op(GenericCircuit &c, gate_t cmp_gate, bool &ok) {
  return cmpOpFromOid(c.getInfos(cmp_gate).first, ok);
}

// Flip operator for "C op agg" <=> "agg flip(op) C"
ComparisonOperator flip_op(ComparisonOperator op) {
  switch (op) {
  case ComparisonOperator::LT:  return ComparisonOperator::GT;
  case ComparisonOperator::LE:  return ComparisonOperator::GE;
  case ComparisonOperator::GT:  return ComparisonOperator::LT;
  case ComparisonOperator::GE:  return ComparisonOperator::LE;
  case ComparisonOperator::EQ:  return ComparisonOperator::EQ;
  case ComparisonOperator::NE:  return ComparisonOperator::NE;
  }
  return op;
}

// Extract a gate_semimod's gate_value extra as a raw string, along with its
// K-gate operand.  Used by the value-as-text HAVING comparison path.
bool semimod_extract_string_and_K(
  GenericCircuit &c,
  gate_t semimod_gate,
  std::string &m_out,
  gate_t &k_gate_out)
{
  if (c.getGateType(semimod_gate) != gate_semimod) return false;

  const auto &w = c.getWires(semimod_gate);
  if (w.size() != 2) return false;

  if (c.getGateType(w[1]) != gate_value) return false;
  m_out = c.getExtra(w[1]);

  k_gate_out = w[0];
  return true;
}

// Extract a constant C encoded as gate_semimod(gate_one, gate_value("C")),
// returning the gate_value's extra as a raw string.  Used by the
// value-as-text HAVING comparison path.
bool extract_constant_string(GenericCircuit &c, gate_t x, std::string &C_out) {
  if (c.getGateType(x) != gate_semimod)
    return false;

  const auto &w = c.getWires(x);
  if (w.size() != 2)
    return false;

  if (c.getGateType(w[0]) != gate_one)
    return false;

  if (c.getGateType(w[1]) != gate_value)
    return false;

  C_out = c.getExtra(w[1]);
  return true;
}

// Whether a comparison side is (or is arithmetic over) an aggregate: descend
// through gate_arith until a gate_agg is found.  Used to recognise the cmp
// gates that the HAVING / WHERE-on-aggregate evaluator must resolve (as
// opposed to RV comparisons, which carry gate_rv leaves instead).
static bool side_has_agg(GenericCircuit &c, gate_t g) {
  gate_type t = c.getGateType(g);
  if (t == gate_agg)
    return true;
  if (t == gate_arith) {
    for (gate_t ch : c.getWires(g))
      if (side_has_agg(c, ch))
        return true;
  }
  return false;
}

// Collect cmp gates in the prov circuit, in post-order: a comparison comes
// after every comparison below it, so that when it is resolved, the
// comparisons its contributors read (a count over rows filtered by a
// comparison on another aggregate) are already in the mapping.
std::vector<gate_t> collect_sp_cmp_gates(GenericCircuit &c, gate_t start) {
  std::vector<gate_t> out;
  std::vector<std::pair<gate_t, bool>> stack;  // (gate, children pushed)
  stack.emplace_back(start, false);

  std::unordered_set<gate_t> seen;

  while (!stack.empty()) {
    auto [cur, expanded] = stack.back();
    stack.pop_back();

    if (!expanded) {
      if (!seen.insert(cur).second) continue;
      stack.emplace_back(cur, true);
      for (gate_t ch : c.getWires(cur))
        if (!seen.count(ch))
          stack.emplace_back(ch, false);
      continue;
    }

    if (c.getGateType(cur) == gate_cmp) {
      const auto &cw = c.getWires(cur);
      if (cw.size() == 2) {
        gate_t L = cw[0];
        gate_t R = cw[1];

        // Any comparison with an aggregate on either side (directly, or under
        // arithmetic): the single-aggregate-vs-constant ones are handled by the
        // fast path, the rest (agg-vs-agg, products of aggregates, c/agg, ...)
        // by the general possible-worlds enumeration.  RV comparisons carry
        // gate_rv leaves instead and are left to the RV evaluator.
        if (side_has_agg(c, L) || side_has_agg(c, R))
          out.push_back(cur);
      }
    }
  }
  return out;
}

} // namespace provsql_having_detail
