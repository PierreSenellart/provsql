/**
 * @file having_semantics.cpp
 * @brief Helper definitions for HAVING-clause provenance evaluation.
 *
 * Defines the small non-template helpers declared in
 * @c provsql_having_detail in @c having_semantics.hpp.  The actual
 * possible-worlds enumeration logic is the @c provsql_having() template
 * in the header.
 */
extern "C" {
#include "postgres.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
}

#include <climits>
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

namespace {
// Parse int from string
int parse_int_strict(const std::string &s, bool &ok) {
  ok = false;
  if (s.empty()) return 0;
  size_t idx = 0;
  try {
    int v = std::stoi(s, &idx);
    if (idx != s.size()) return 0;
    ok = true;
    return v;
  } catch (...) {
    return 0;
  }
}

// Parse double from string
double parse_double_strict(const std::string &s, bool &ok) {
  ok = false;
  if (s.empty()) return 0.0;
  size_t idx = 0;
  try {
    double v = std::stod(s, &idx);
    if (idx != s.size()) return 0.0;
    ok = true;
    return v;
  } catch (...) {
    return 0.0;
  }
}
} // namespace

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

bool semimod_extract_M_and_K(
  GenericCircuit &c,
  gate_t semimod_gate,
  int &m_out,
  gate_t &k_gate_out)
{
  if (c.getGateType(semimod_gate) != gate_semimod) return false;

  const auto &w = c.getWires(semimod_gate);
  if (w.size() != 2) return false;

  if (c.getGateType(w[1]) != gate_value) return false;
  bool ok = false;
  m_out = parse_int_strict(c.getExtra(w[1]), ok);
  if (!ok) return false;

  k_gate_out = w[0];
  return true;
}

// String sibling of semimod_extract_M_and_K: extracts the gate_value's
// extra as a raw string instead of parsing it as an int.  Used by the
// agg_token-to-text comparison path.
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

// Extract constant C from possible encodings:
// - gate_value("C")
// - gate_semimod(C, gate_one)
// - gate_semimod(gate_one, C)
bool extract_constant_C(GenericCircuit &c, gate_t x, int &C_out) {
  if (c.getGateType(x) != gate_semimod)
    return false;

  const auto &w = c.getWires(x);
  if (w.size() != 2)
    return false;

  if (c.getGateType(w[0]) != gate_one)
    return false;

  if (c.getGateType(w[1]) != gate_value)
    return false;

  bool ok = false;
  int v = parse_int_strict(c.getExtra(w[1]), ok);
  if (!ok)
    return false;

  C_out = v;
  return true;
}

// Float8 sibling of extract_constant_C: parses a gate_value's extra as
// double precision instead of int.  Used by the continuous-distributions
// path where the right-hand side of a gate_cmp is a float literal.
bool extract_constant_double(GenericCircuit &c, gate_t x, double &C_out) {
  if (c.getGateType(x) != gate_value)
    return false;

  bool ok = false;
  double v = parse_double_strict(c.getExtra(x), ok);
  if (!ok)
    return false;

  C_out = v;
  return true;
}

// String sibling of extract_constant_C: returns the gate_value's extra as a
// raw string instead of parsing it as an int.  Used by the agg_token-to-text
// comparison path.
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

// Collect cmp gates in the prov circuit
std::vector<gate_t> collect_sp_cmp_gates(GenericCircuit &c, gate_t start) {
  std::vector<gate_t> out;
  std::vector<gate_t> stack;
  stack.push_back(start);

  std::unordered_set<gate_t> seen;

  while (!stack.empty()) {
    gate_t cur = stack.back();
    stack.pop_back();

    if (!seen.insert(cur).second) continue;

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

    const auto &w = c.getWires(cur);
    for (gate_t ch : w) stack.push_back(ch);
  }
  return out;
}

} // namespace provsql_having_detail
