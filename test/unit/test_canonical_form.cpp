#include <cassert>
#include <cmath>
#include <iostream>
#include <utility>

#include "CircuitCanonicalForm.h"
#include "CanonicalCache.h"
#include "TreeDecomposition.h"
#include "dDNNFTreeDecompositionBuilder.h"

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

static void testDomination() {
  BooleanCircuit c1;
  gate_t x = c1.setGate("x", BooleanGate::IN, 0.5);
  gate_t y = c1.setGate("y", BooleanGate::IN, 0.5);
  gate_t z = c1.setGate("z", BooleanGate::IN, 0.5);
  gate_t g1 = c1.setGate(BooleanGate::AND);
  c1.addWire(g1, x); c1.addWire(g1, y);
  gate_t g2 = c1.setGate(BooleanGate::AND);
  c1.addWire(g2, y); c1.addWire(g2, z);
  gate_t o1 = c1.setGate(BooleanGate::OR);
  c1.addWire(o1, g1); c1.addWire(o1, g2);
  assert(maximalIslandRoots(c1, o1).empty());

  BooleanCircuit c2;
  gate_t x2 = c2.setGate("x", BooleanGate::IN, 0.5);
  gate_t y2 = c2.setGate("y", BooleanGate::IN, 0.5);
  gate_t z2 = c2.setGate("z", BooleanGate::IN, 0.5);
  gate_t w2 = c2.setGate("w", BooleanGate::IN, 0.5);
  gate_t h1 = c2.setGate(BooleanGate::AND);
  c2.addWire(h1, x2); c2.addWire(h1, y2);
  gate_t h2 = c2.setGate(BooleanGate::AND);
  c2.addWire(h2, z2); c2.addWire(h2, w2);
  gate_t o2 = c2.setGate(BooleanGate::OR);
  c2.addWire(o2, h1); c2.addWire(o2, h2);
  assert(maximalIslandRoots(c2, o2).size() == 2);

  IslandKey k2 = computeIslandKey(c2, o2);
  assert(k2.residual.form == "(OR V V)");
  assert(k2.islands.size() == 2);
  assert(k2.islands[0].form == "(AND V V)");
  assert(k2.islands[1].form == "(AND V V)");

  IslandKey k1 = computeIslandKey(c1, o1);
  assert(k1.islands.empty());
  assert(k1.residual.form == computeCanonicalForm(c1, o1).form);
  assert(k1.residual.form != k2.residual.form);
}

static void testSharedIsland() {
  BooleanCircuit c;
  gate_t a = c.setGate("a", BooleanGate::IN, 0.5);
  gate_t b = c.setGate("b", BooleanGate::IN, 0.5);
  gate_t g = c.setGate(BooleanGate::AND);
  c.addWire(g, a); c.addWire(g, b);
  gate_t n = c.setGate(BooleanGate::NOT);
  c.addWire(n, g);
  gate_t o = c.setGate(BooleanGate::OR);
  c.addWire(o, g); c.addWire(o, n);
  auto roots = maximalIslandRoots(c, o);
  assert(roots.size() == 1);
  assert(roots[0] == g);
  IslandKey k = computeIslandKey(c, o);
  assert(k.residual.form == "(OR V (NOT #2))");
  assert(k.islands.size() == 1);
  assert(k.islands[0].form == "(AND V V)");
}

static void testCacheEndToEnd() {
  auto compile = [](const BooleanCircuit &cc, gate_t g) {
    TreeDecomposition td(cc);
    return dDNNFTreeDecompositionBuilder{cc, g, td}.build();
  };

  auto build = [](const std::string &prefix, double pa, double pb, double pc,
                  gate_t &root) {
    BooleanCircuit c;
    gate_t a = c.setGate(prefix + "a", BooleanGate::IN, pa);
    gate_t b = c.setGate(prefix + "b", BooleanGate::IN, pb);
    gate_t cc = c.setGate(prefix + "c", BooleanGate::IN, pc);
    gate_t g1 = c.setGate(BooleanGate::AND);
    c.addWire(g1, a); c.addWire(g1, b);
    gate_t g2 = c.setGate(BooleanGate::AND);
    c.addWire(g2, a); c.addWire(g2, cc);
    root = c.setGate(BooleanGate::OR);
    c.addWire(root, g1); c.addWire(root, g2);
    return c;
  };

  auto expected = [](double pa, double pb, double pc) {
    return pa * (pb + pc - pb * pc);
  };

  CanonicalCache::instance().reset();

  gate_t r1;
  BooleanCircuit c1 = build("t1_", 0.5, 0.4, 0.3, r1);
  assert(canonicalCacheEligible(c1, r1));
  bool compiled1 = false;
  double p1 = canonicalCachedProbability(c1, r1, compile, &compiled1);
  assert(compiled1);
  assert(std::fabs(p1 - expected(0.5, 0.4, 0.3)) < 1e-12);

  gate_t r2;
  BooleanCircuit c2 = build("t2_", 0.9, 0.2, 0.7, r2);
  bool compiled2 = true;
  double p2 = canonicalCachedProbability(c2, r2, compile, &compiled2);
  assert(!compiled2);
  assert(std::fabs(p2 - expected(0.9, 0.2, 0.7)) < 1e-12);

  auto st = CanonicalCache::instance().stats();
  assert(st.hits > 0);
  assert(st.inserts > 0);

  BooleanCircuit c3;
  gate_t a3 = c3.setGate("u", BooleanGate::IN, 0.25);
  gate_t b3 = c3.setGate("v", BooleanGate::IN, 0.5);
  gate_t w3 = c3.setGate("w", BooleanGate::IN, 0.5);
  gate_t g3 = c3.setGate(BooleanGate::AND);
  c3.addWire(g3, a3); c3.addWire(g3, b3);
  gate_t n3 = c3.setGate(BooleanGate::NOT);
  c3.addWire(n3, w3);
  gate_t o3 = c3.setGate(BooleanGate::OR);
  c3.addWire(o3, g3); c3.addWire(o3, n3);
  bool compiled3 = false;
  double p3 = canonicalCachedProbability(c3, o3, compile, &compiled3);
  assert(std::fabs(p3 - (1 - (1 - 0.125) * 0.5)) < 1e-12);

  BooleanCircuit c4;
  gate_t a4 = c4.setGate("u2", BooleanGate::IN, 0.6);
  gate_t b4 = c4.setGate("v2", BooleanGate::IN, 0.8);
  gate_t w4 = c4.setGate("w2", BooleanGate::IN, 0.1);
  gate_t g4 = c4.setGate(BooleanGate::AND);
  c4.addWire(g4, a4); c4.addWire(g4, b4);
  gate_t n4 = c4.setGate(BooleanGate::NOT);
  c4.addWire(n4, w4);
  gate_t o4 = c4.setGate(BooleanGate::OR);
  c4.addWire(o4, g4); c4.addWire(o4, n4);
  bool compiled4 = true;
  double p4 = canonicalCachedProbability(c4, o4, compile, &compiled4);
  assert(!compiled4);
  assert(std::fabs(p4 - (1 - (1 - 0.48) * (1 - 0.9))) < 1e-12);
}

static void testResidualMatchesCircuit() {
  auto compile = [](const BooleanCircuit &cc, gate_t g) {
    TreeDecomposition td(cc);
    return dDNNFTreeDecompositionBuilder{cc, g, td}.build();
  };

  auto buildOr = [](double pu, double pv, gate_t &root) {
    BooleanCircuit c;
    gate_t u = c.setGate("m_u", BooleanGate::IN, pu);
    gate_t v = c.setGate("m_v", BooleanGate::IN, pv);
    root = c.setGate(BooleanGate::OR);
    c.addWire(root, u); c.addWire(root, v);
    return c;
  };

  auto buildOrOfAnds = [](double px, double py, double pz, double pw,
                          gate_t &root) {
    BooleanCircuit c;
    gate_t x = c.setGate("m_x", BooleanGate::IN, px);
    gate_t y = c.setGate("m_y", BooleanGate::IN, py);
    gate_t z = c.setGate("m_z", BooleanGate::IN, pz);
    gate_t w = c.setGate("m_w", BooleanGate::IN, pw);
    gate_t h1 = c.setGate(BooleanGate::AND);
    c.addWire(h1, x); c.addWire(h1, y);
    gate_t h2 = c.setGate(BooleanGate::AND);
    c.addWire(h2, z); c.addWire(h2, w);
    root = c.setGate(BooleanGate::OR);
    c.addWire(root, h1); c.addWire(root, h2);
    return c;
  };

  CanonicalCache::instance().reset();
  gate_t r1;
  BooleanCircuit c1 = buildOr(0.3, 0.6, r1);
  IslandKey k1 = computeIslandKey(c1, r1);
  assert(k1.islands.empty());
  assert(k1.residual.form == "(OR V V)");
  double p1 = canonicalCachedProbability(c1, r1, compile);
  assert(std::fabs(p1 - (1 - 0.7 * 0.4)) < 1e-12);

  gate_t r2;
  BooleanCircuit c2 = buildOrOfAnds(0.5, 0.4, 0.9, 0.2, r2);
  IslandKey k2 = computeIslandKey(c2, r2);
  assert(k2.islands.size() == 2);
  assert(k2.residual.form == "(OR V V)");
  CanonicalCacheStats before = CanonicalCache::instance().stats();
  double p2 = canonicalCachedProbability(c2, r2, compile);
  CanonicalCacheStats after = CanonicalCache::instance().stats();
  assert(after.inserts == before.inserts + 1);
  assert(after.hits == before.hits + 2);
  assert(std::fabs(p2 - (1 - (1 - 0.2) * (1 - 0.18))) < 1e-12);

  CanonicalCache::instance().reset();
  gate_t r3;
  BooleanCircuit c3 = buildOrOfAnds(0.7, 0.1, 0.3, 0.8, r3);
  double p3 = canonicalCachedProbability(c3, r3, compile);
  assert(std::fabs(p3 - (1 - (1 - 0.07) * (1 - 0.24))) < 1e-12);

  gate_t r4;
  BooleanCircuit c4 = buildOr(0.25, 0.45, r4);
  bool compiled4 = true;
  double p4 = canonicalCachedProbability(c4, r4, compile, &compiled4);
  assert(!compiled4);
  assert(std::fabs(p4 - (1 - 0.75 * 0.55)) < 1e-12);
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
  assert(maximalIslandRoots(c, prev).size() == 1);
}

void provsql_tdkc_poll() {}

static void testMoveDoesNotCopy() {
  dDNNF source;
  gate_t a = source.setGate("a", BooleanGate::IN, 0.5);
  gate_t b = source.setGate("b", BooleanGate::IN, 0.5);
  gate_t g = source.setGate(BooleanGate::AND);
  source.addWire(g, a);
  source.addWire(g, b);
  source.setRoot(g);

  dDNNF target = std::move(source);
  assert(target.getNbGates() == 3);
  assert(source.getNbGates() == 0);

  CanonicalCache::Entry first;
  first.dd = std::move(target);
  CanonicalCache::Entry second = std::move(first);
  assert(second.dd.getNbGates() == 3);
  assert(first.dd.getNbGates() == 0);
}

static void testAdmits() {
  CanonicalCache::instance().reset();
  CanonicalCache &cache = CanonicalCache::instance();
  assert(cache.admits(1u << 20));
  assert(cache.stats().rejections == 0);
  assert(!cache.admits(size_t{1} << 40));
  assert(cache.stats().rejections == 1);
  CanonicalCache::instance().reset();
}

int main() {
  testMoveDoesNotCopy();
  testAdmits();
  testWorkedExample();
  testRenaming();
  testEssentialTie();
  testDomination();
  testSharedIsland();
  testCacheEndToEnd();
  testResidualMatchesCircuit();
  testDeepChain();
  std::cout << "all tests passed" << std::endl;
  return 0;
}
