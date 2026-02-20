#ifndef PROVSQL_HAVING_SEMANTICS_HPP
#define PROVSQL_HAVING_SEMANTICS_HPP
#include <string>
#include <unordered_map>

#include "GenericCircuit.hpp"
#include "BooleanCircuit.h"
#include "semiring/BoolExpr.h"
#include "semiring/Why.h"

void provsql_try_having_formula(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, std::string> &mapping
  );
void provsql_try_having_counting(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, unsigned> &mapping
  );
void provsql_try_having_why(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, semiring::why_provenance_t> &mapping
  );
void provsql_try_having_boolexpr(
  GenericCircuit &c,
  semiring::BoolExpr &be,
  gate_t g,
  std::unordered_map<gate_t, gate_t> &mapping
  );
void provsql_try_having_boolean(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, bool> &mapping
  );

#endif
