#ifndef BOOLEXPR_H
#define BOOLEXPR_H

#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include "Semiring.h"
#include "../BooleanCircuit.h"

namespace semiring {
class BoolExpr : public Semiring<gate_t> {
using value_t = gate_t;

BooleanCircuit &c;
const gate_t ZERO, ONE;

public:
BoolExpr(BooleanCircuit &bc) : c(bc), ZERO(plus({})), ONE(times({})) {
}

value_type zero() const override {
  return ZERO;
}

value_type one() const override {
  return ONE;
}

value_type plus(const std::vector<value_type> &vec) const override {
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

};
}

#endif // BOOLEXPR_H
