/**
 * @file semiring/BoolExpr.h
 * @brief Boolean-expression (lineage formula) semiring.
 *
 * The @c BoolExpr semiring represents provenance as a Boolean circuit
 * rather than a scalar value.  Each semiring value is a @c gate_t
 * identifier inside a shared @c BooleanCircuit.  The semiring operations
 * create new gates in that circuit:
 *
 * - @c zero()   → a fresh OR gate with no children (always @c false)
 * - @c one()    → a fresh AND gate with no children (always @c true)
 * - @c plus()   → an OR gate whose children are the operands
 * - @c times()  → an AND gate whose children are the operands
 * - @c monus()  → an AND gate combining @f$x@f$ with a NOT of @f$y@f$
 * - @c delta()  → identity
 *
 * The result of evaluating a circuit over this semiring is the root gate
 * of a new Boolean circuit that encodes the provenance formula.  This
 * circuit can then be compiled to a d-DNNF for probability computation.
 *
 * The semiring is absorptive: duplicate children of OR/AND gates are
 * deduplicated during gate construction.
 */
#ifndef BOOLEXPR_H
#define BOOLEXPR_H

#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include "Semiring.h"
#include "../BooleanCircuit.h"

namespace semiring {
/**
 * @brief Provenance-as-Boolean-circuit semiring.
 *
 * The carrier type is @c gate_t (a gate identifier in @c BooleanCircuit).
 * Evaluating the provenance circuit over this semiring constructs a new
 * Boolean circuit expressing the provenance formula.
 */
class BoolExpr : public Semiring<gate_t> {
using value_t = gate_t; ///< Carrier type: a gate ID in the target BooleanCircuit

BooleanCircuit &c;      ///< The Boolean circuit being constructed
const gate_t ZERO; ///< Pre-allocated zero gate (OR with no children)
const gate_t ONE;  ///< Pre-allocated one gate (AND with no children)

public:
/**
 * @brief Construct a BoolExpr semiring over the given circuit.
 * @param bc  The Boolean circuit in which semiring operations create new gates.
 */
BoolExpr(BooleanCircuit &bc) : c(bc), ZERO(c.setGate(BooleanGate::OR)), ONE(c.setGate(BooleanGate::AND)) {
}

value_type zero() const override {
  return ZERO;
}

value_type one() const override {
  return ONE;
}

value_type plus(const std::vector<value_type> &vec) const override {
  if(vec.empty())
    return ZERO;

  auto g = c.setGate(BooleanGate::OR);

  std::set<gate_t> seen;
  for (const auto &h : vec) {
    if(seen.find(h)!=seen.end())
      continue;
    seen.insert(h);
    c.addWire(g, h);
  }
  return g;
}

value_type times(const std::vector<value_type> &vec) const override {
  if(vec.empty())
    return ONE;

  auto g = c.setGate(BooleanGate::AND);

  std::set<gate_t> seen;
  for (const auto &h : vec) {
    if(seen.find(h)!=seen.end())
      continue;
    seen.insert(h);
    c.addWire(g, h);
  }
  return g;
}

virtual value_type monus(value_type x, value_type y) const override {
  auto g2 = c.setGate(BooleanGate::NOT);
  c.addWire(g2,y);

  if(x==ONE)
    return g2;
  else {
    auto g1 = c.setGate(BooleanGate::AND);
    c.addWire(g1,x);
    c.addWire(g1,g2);
    return g1;
  }
}

value_type delta(value_type x) const override {
  return x;
}

virtual bool absorptive() const override {
  return true;
}

};
}

#endif // BOOLEXPR_H
