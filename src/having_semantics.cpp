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
#include "utils/lsyscache.h"
}

#include <string>
#include <unordered_set>
#include <vector>

#include "having_semantics.hpp"

namespace provsql_having_detail {

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
} // namespace

// Map a cmp gate's Postgres operator to subset.cpp's ComparisonOperator
ComparisonOperator map_cmp_op(GenericCircuit &c, gate_t cmp_gate, bool &ok) {
  ok = false;
  auto infos = c.getInfos(cmp_gate);

  char *opname = get_opname(infos.first); // palloc'd string or NULL
  if (!opname) return ComparisonOperator::EQ;

  std::string s(opname);
  pfree(opname);

  ok = true;
  if (s == "=") return ComparisonOperator::EQ;
  if (s == "<>") return ComparisonOperator::NE;
  if (s == "<") return ComparisonOperator::LT;
  if (s == "<=") return ComparisonOperator::LE;
  if (s == ">") return ComparisonOperator::GT;
  if (s == ">=") return ComparisonOperator::GE;

  ok = false;
  return ComparisonOperator::EQ;
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

        int tmpC = 0;
        bool is_candidate =
          (c.getGateType(L) == gate_agg && extract_constant_C(c, R, tmpC)) ||
          (c.getGateType(R) == gate_agg && extract_constant_C(c, L, tmpC));

        if (is_candidate)
          out.push_back(cur);
      }
    }

    const auto &w = c.getWires(cur);
    for (gate_t ch : w) stack.push_back(ch);
  }
  return out;
}

} // namespace provsql_having_detail
