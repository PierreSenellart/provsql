extern "C" {
#include "postgres.h"
#include "utils/lsyscache.h"
}

#include <vector>
#include <string>
#include <unordered_map>

#include "having_semantics.hpp"
#include "subset.hpp"
#include "semiring/Boolean.h"
#include "semiring/Counting.h"
#include "semiring/Formula.h"
#include "semiring/Why.h"
namespace {
// Parse int from string 
static int parse_int_strict(const std::string &s, bool &ok) {
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

// Map a cmp gate’s Postgres operator to subset.cpp’s ComparisonOp
static ComparisonOp map_cmp_op(GenericCircuit &c, gate_t cmp_gate, bool &ok) {
  ok = false;
  auto infos = c.getInfos(cmp_gate);

  char *opname = get_opname(infos.first); // palloc'd string or NULL
  if (!opname) return ComparisonOp::EQ;

  std::string s(opname);
  pfree(opname);

  ok = true;
  if (s == "=")  return ComparisonOp::EQ;
  if (s == "<>") return ComparisonOp::NEQ;
  if (s == "<")  return ComparisonOp::LT;
  if (s == "<=") return ComparisonOp::LE;
  if (s == ">")  return ComparisonOp::GT;
  if (s == ">=") return ComparisonOp::GE;

  ok = false;
  return ComparisonOp::EQ;
}

// Flip operator for “C op agg”  <=>  “agg flip(op) C”
static ComparisonOp flip_op(ComparisonOp op) {
  switch (op) {
    case ComparisonOp::LT:  return ComparisonOp::GT;
    case ComparisonOp::LE:  return ComparisonOp::GE;
    case ComparisonOp::GT:  return ComparisonOp::LT;
    case ComparisonOp::GE:  return ComparisonOp::LE;
    case ComparisonOp::EQ:  return ComparisonOp::EQ;
    case ComparisonOp::NEQ: return ComparisonOp::NEQ;
  }
  return op;
}


static bool semimod_extract_M_and_K(
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
static bool extract_constant_C(GenericCircuit &c, gate_t x, int &C_out) {

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

} // namespace

// ----------------------------------------------------
// Generic implementation of possible-world
// semantics for HAVING queries for any semiring defined
// in src/semiring/
// --------------------------------------------------
template <typename SemiringT, typename MapT>
static bool try_having_impl(
  GenericCircuit &c,
  gate_t g,
  MapT &mapping,
  typename SemiringT::value_type &out)
{
  SemiringT S;

  // Recognize: times(delta(...), cmp(...)) 
  gate_t root = g;
  gate_t cmp_gate{};
  bool found = false;

  if (c.getGateType(root) == gate_times) {
    const auto &w = c.getWires(root);
    if (w.size() == 2) {
      gate_t a =w[0];
      gate_t b = w[1];

      if (c.getGateType(a) == gate_cmp && c.getGateType(b) == gate_delta) {
        cmp_gate = a; found = true;
      } else if (c.getGateType(b) == gate_cmp && c.getGateType(a) == gate_delta) {
        cmp_gate = b; found = true;
      }
    }
  }

  if (!found) return false;

  const auto &cw = c.getWires(cmp_gate);
  if (cw.size() != 2) return false;

  gate_t L = cw[0];
  gate_t R = cw[1];

  bool okop = false;
  ComparisonOp op = map_cmp_op(c, cmp_gate, okop);
  if (!okop) return false;

  // possible world provenance expressiomn builder:
  // - take an agg gate, extract (m_i, k_i)
  // - enumerate valid worlds by sum(m_i) op C
  // - compute: ⊕_W ( (⊗_{i in W} k_i) ⊗ (1 ⊖ ⊕_{i not in W} k_i) )
  auto build_from = [&](gate_t agg_side, gate_t const_side, ComparisonOp effective_op) -> bool {
    int C = 0;
    if (!extract_constant_C(c, const_side, C)) return false;

    
    if (c.getGateType(agg_side) != gate_agg) return false;

    const auto &children = c.getWires(agg_side);

    std::vector<int> mvals;
    mvals.reserve(children.size());

    std::vector<typename SemiringT::value_type> kvals;
    kvals.reserve(children.size());

    for (gate_t ch : children) {
      
      if (c.getGateType(ch) != gate_semimod) return false;

      int m = 0;
      gate_t k_gate{};
      if (!semimod_extract_M_and_K(c, ch, m, k_gate)) return false;

      
      auto kval = c.evaluate<SemiringT>(k_gate, mapping);

      mvals.push_back(m);
      kvals.push_back(std::move(kval));
    }

    std::vector<uint64_t> worlds = enumerate_valid_worlds(mvals, C, effective_op);

    if (worlds.empty()) {
      out = S.zero();
      return true;
    }

    std::vector<typename SemiringT::value_type> disjuncts;
    disjuncts.reserve(worlds.size());

    const size_t n = kvals.size();

    for (uint64_t mask : worlds) {
      std::vector<typename SemiringT::value_type> present;
      std::vector<typename SemiringT::value_type> missing;
      present.reserve(n);
      missing.reserve(n);

      for (size_t i = 0; i < n; ++i) {
        if (mask & (1ULL << i)) present.push_back(kvals[i]);
        else                    missing.push_back(kvals[i]);
      }

      // product over present; 
      auto present_prod = S.times(present);

      if (missing.empty()) {
        disjuncts.push_back(std::move(present_prod));
      } else {
        auto missing_sum = S.plus(missing);
        auto monus_factor = S.monus(S.one(), missing_sum);
        auto term = S.times(std::vector<typename SemiringT::value_type>{present_prod, monus_factor});
        disjuncts.push_back(std::move(term));
      }
    }

    out = S.plus(disjuncts);
    return true;
  };

  // Case A: agg op const
  if (c.getGateType(L) == gate_agg) {
    if (build_from(L, R, op)) return true;
  }

  // Case B: const op agg  
  if (c.getGateType(R) == gate_agg) {
    if (build_from(R, L, flip_op(op))) return true;
  }

  return false;
}

//-------------------------
// Public wrappers
//----------------------------- 
bool provsql_try_having_formula(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, std::string> &mapping,
  std::string &out)
{
  return try_having_impl<semiring::Formula>(c, g, mapping, out);
}

bool provsql_try_having_counting(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, unsigned> &mapping,
  unsigned &out)
{
  return try_having_impl<semiring::Counting>(c, g, mapping, out);
}

bool provsql_try_having_why(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, semiring::why_provenance_t> &mapping,
  semiring::why_provenance_t &out)
{
  return try_having_impl<semiring::Why>(c, g, mapping, out);
}

bool provsql_try_having_boolean(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, bool> &mapping,
  bool &out)
{
  return try_having_impl<semiring::Boolean>(c, g, mapping, out);
}
