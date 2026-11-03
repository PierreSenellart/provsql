#include <cassert>
#include <iostream>
#include <utility>

#include "CircuitCanonicalForm.h"

using namespace provsql;

static void testWorkedExample() {
  BooleanCircuit e1;
  gate_t a1 = e1.setGate("a", BooleanGate::IN, 0.5);
  gate_t b1 = e1.setGate("b", BooleanGate::IN, 0.5);
  gate_t c1 = e1.setGate("c", BooleanGate::IN, 0.5);
  gate_t and11 = e1.setGate(BooleanGate::AND);
  e1.addWire(and11, a1); e1.addWire(and11, b1);
  gate_t and12 = e1.setGate(BooleanGate::AND);
  e1.addWire(and12, a1); e1.addWire(and12, c1);
  gate_t or1 = e1.setGate(BooleanGate::OR);
  e1.addWire(or1, and11); e1.addWire(or1, and12);

  BooleanCircuit e2;
  gate_t a2 = e2.setGate("a", BooleanGate::IN, 0.5);
  gate_t b2 = e2.setGate("b", BooleanGate::IN, 0.5);
  gate_t c2 = e2.setGate("c", BooleanGate::IN, 0.5);
  gate_t and21 = e2.setGate(BooleanGate::AND);
  e2.addWire(and21, a2); e2.addWire(and21, b2);
  gate_t and22 = e2.setGate(BooleanGate::AND);
  e2.addWire(and22, c2); e2.addWire(and22, b2);
  gate_t or2 = e2.setGate(BooleanGate::OR);
  e2.addWire(or2, and21); e2.addWire(or2, and22);

  CanonicalForm f1 = computeCanonicalForm(e1, or1);
  CanonicalForm f2 = computeCanonicalForm(e2, or2);
  assert(f1.form == "(OR (AND V V) (AND V #4))");
  assert(f1.form == f2.form);
  assert(f1.hash == f2.hash);
  assert(f1.leaves.size() == 3);

  BooleanCircuit e3;
  gate_t a3 = e3.setGate("a", BooleanGate::IN, 0.5);
  gate_t b3 = e3.setGate("b", BooleanGate::IN, 0.5);
  gate_t c3 = e3.setGate("c", BooleanGate::IN, 0.5);
  gate_t and31 = e3.setGate(BooleanGate::AND);
  e3.addWire(and31, a3); e3.addWire(and31, c3);
  gate_t and32 = e3.setGate(BooleanGate::AND);
  e3.addWire(and32, a3); e3.addWire(and32, b3);
  gate_t or3 = e3.setGate(BooleanGate::OR);
  e3.addWire(or3, and31); e3.addWire(or3, and32);
  CanonicalForm f3 = computeCanonicalForm(e3, or3);
  assert(f3.form == f1.form);
}

static void testRenaming() {
  BooleanCircuit e1, e2;
  for(int variant = 0; variant < 2; ++variant) {
    BooleanCircuit &e = variant ? e2 : e1;
    std::string p = variant ? "renamed_" : "orig_";
    gate_t x = e.setGate(p + "x", BooleanGate::IN, 0.3);
    gate_t y = e.setGate(p + "y", BooleanGate::IN, 0.7);
    gate_t n = e.setGate(BooleanGate::NOT);
    e.addWire(n, y);
    gate_t g = e.setGate(BooleanGate::AND);
    e.addWire(g, x); e.addWire(g, n);
    gate_t o = e.setGate(BooleanGate::OR);
    e.addWire(o, g); e.addWire(o, x);
  }
  CanonicalForm f1 = computeCanonicalForm(e1, gate_t{4});
  CanonicalForm f2 = computeCanonicalForm(e2, gate_t{4});
  assert(f1.form == f2.form);
}

static void testEssentialTie() {
  auto build = [](const int pairs[4][2]) {
    auto c = new BooleanCircuit();
    gate_t in[4];
    const char *names[4] = {"a","b","c","d"};
    for(int i = 0; i < 4; ++i)
      in[i] = c->setGate(names[i], BooleanGate::IN, 0.5);
    gate_t ors = c->setGate(BooleanGate::OR);
    for(int k = 0; k < 4; ++k) {
      gate_t g = c->setGate(BooleanGate::AND);
      c->addWire(g, in[pairs[k][0]]);
      c->addWire(g, in[pairs[k][1]]);
      c->addWire(ors, g);
    }
    return std::make_pair(c, ors);
  };
  const int A[4][2] = {{0,1},{1,2},{2,3},{3,0}};
  const int B[4][2] = {{0,1},{3,0},{2,3},{1,2}};
  auto [ca, ra] = build(A);
  auto [cb, rb] = build(B);
  CanonicalForm fa = computeCanonicalForm(*ca, ra);
  CanonicalForm fb = computeCanonicalForm(*cb, rb);
  assert(fa.form == "(OR (AND V V) (AND #4 V) (AND #6 V) (AND #8 #3))");
  assert(fb.form == "(OR (AND V V) (AND V #3) (AND V #6) (AND #4 #8))");
  delete ca; delete cb;
}

static void testDeepChain() {
  BooleanCircuit c;
  gate_t prev = c.setGate("leaf", BooleanGate::IN, 0.5);
  for(int i = 0; i < 20000; ++i) {
    gate_t n = c.setGate(BooleanGate::NOT);
    c.addWire(n, prev);
    prev = n;
  }
  CanonicalForm f = computeCanonicalForm(c, prev);
  assert(f.form.size() > 20000);
}

void provsql_tdkc_poll() {}

int main() {
  testWorkedExample();
  testRenaming();
  testEssentialTie();
  testDeepChain();
  std::cout << "all tests passed" << std::endl;
  return 0;
}
