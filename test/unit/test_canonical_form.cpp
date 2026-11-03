#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "CircuitCanonicalForm.h"
#include "ProbCache.h"
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

  ProbCache::instance().reset();

  gate_t r1;
  BooleanCircuit c1 = build("t1_", 0.5, 0.4, 0.3, r1);
  assert(probCacheEligible(c1, r1));
  bool compiled1 = false;
  double p1 = probCachedProbability(c1, r1, compile, &compiled1);
  assert(compiled1);
  assert(std::fabs(p1 - expected(0.5, 0.4, 0.3)) < 1e-12);

  gate_t r2;
  BooleanCircuit c2 = build("t2_", 0.9, 0.2, 0.7, r2);
  bool compiled2 = true;
  double p2 = probCachedProbability(c2, r2, compile, &compiled2);
  assert(!compiled2);
  assert(std::fabs(p2 - expected(0.9, 0.2, 0.7)) < 1e-12);

  auto st = ProbCache::instance().stats();
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
  double p3 = probCachedProbability(c3, o3, compile, &compiled3);
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
  double p4 = probCachedProbability(c4, o4, compile, &compiled4);
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

  ProbCache::instance().reset();
  gate_t r1;
  BooleanCircuit c1 = buildOr(0.3, 0.6, r1);
  IslandKey k1 = computeIslandKey(c1, r1);
  assert(k1.islands.empty());
  assert(k1.residual.form == "(OR V V)");
  double p1 = probCachedProbability(c1, r1, compile);
  assert(std::fabs(p1 - (1 - 0.7 * 0.4)) < 1e-12);

  gate_t r2;
  BooleanCircuit c2 = buildOrOfAnds(0.5, 0.4, 0.9, 0.2, r2);
  IslandKey k2 = computeIslandKey(c2, r2);
  assert(k2.islands.size() == 2);
  assert(k2.residual.form == "(OR V V)");
  ProbCacheStats before = ProbCache::instance().stats();
  double p2 = probCachedProbability(c2, r2, compile);
  ProbCacheStats after = ProbCache::instance().stats();
  assert(after.inserts == before.inserts + 1);
  assert(after.hits == before.hits + 2);
  assert(std::fabs(p2 - (1 - (1 - 0.2) * (1 - 0.18))) < 1e-12);

  ProbCache::instance().reset();
  gate_t r3;
  BooleanCircuit c3 = buildOrOfAnds(0.7, 0.1, 0.3, 0.8, r3);
  double p3 = probCachedProbability(c3, r3, compile);
  assert(std::fabs(p3 - (1 - (1 - 0.07) * (1 - 0.24))) < 1e-12);

  gate_t r4;
  BooleanCircuit c4 = buildOr(0.25, 0.45, r4);
  bool compiled4 = true;
  double p4 = probCachedProbability(c4, r4, compile, &compiled4);
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

static GateSet subByDefinition(const BooleanCircuit &c, gate_t root,
                               const GateSet &nodes, gate_t g) {
  GateSet avoid;
  if(g != root) {
    std::vector<gate_t> stack{root};
    avoid.insert(root);
    while(!stack.empty()) {
      gate_t x = stack.back();
      stack.pop_back();
      for(gate_t u : c.getWires(x))
        if(u != g && avoid.insert(u).second)
          stack.push_back(u);
    }
  }
  GateSet sub;
  for(gate_t u : nodes)
    if(!avoid.count(u))
      sub.insert(u);
  return sub;
}

static std::vector<gate_t> frontierByDefinition(const BooleanCircuit &c,
                                                gate_t root) {
  const GateSet nodes = reachableNodes(c, root);
  std::vector<gate_t> roots;
  std::vector<GateSet> subs;
  for(gate_t g : nodes) {
    const BooleanGate t = c.getGateType(g);
    if(g == root || c.getWires(g).empty()
       || (t != BooleanGate::AND && t != BooleanGate::OR
           && t != BooleanGate::NOT))
      continue;
    GateSet sub = subByDefinition(c, root, nodes, g);
    bool island = true;
    for(gate_t w : sub)
      for(gate_t u : c.getWires(w))
        if(!sub.count(u))
          island = false;
    if(island) {
      roots.push_back(g);
      subs.push_back(std::move(sub));
    }
  }
  std::vector<gate_t> fstar;
  for(size_t i = 0; i < roots.size(); ++i) {
    bool maximal = true;
    for(size_t j = 0; j < roots.size(); ++j)
      if(j != i && subs[j].count(roots[i]))
        maximal = false;
    if(maximal)
      fstar.push_back(roots[i]);
  }
  std::sort(fstar.begin(), fstar.end());
  return fstar;
}

static void testFrontierAgainstDefinitions() {
  std::mt19937 rng(20261103);
  size_t nonempty = 0;
  const size_t trials = 4000;
  for(size_t trial = 0; trial < trials; ++trial) {
    BooleanCircuit c;
    std::vector<gate_t> made;
    const int inputs = 1 + static_cast<int>(rng() % 5);
    for(int i = 0; i < inputs; ++i)
      made.push_back(c.setGate("x" + std::to_string(i), BooleanGate::IN, 0.5));
    const int gates = 1 + static_cast<int>(rng() % 25);
    for(int i = 0; i < gates; ++i) {
      const unsigned kind = rng() % 3;
      const BooleanGate t = kind == 0 ? BooleanGate::AND
                            : kind == 1 ? BooleanGate::OR : BooleanGate::NOT;
      gate_t g = c.setGate(t);
      const int arity = t == BooleanGate::NOT ? 1 : 1 + static_cast<int>(rng() % 4);
      const size_t window = rng() % 2 == 0 ? made.size()
                            : std::min<size_t>(made.size(), 3);
      for(int a = 0; a < arity; ++a)
        c.addWire(g, made[made.size() - 1 - rng() % window]);
      made.push_back(g);
    }
    const gate_t root = made.back();
    std::vector<gate_t> got = maximalIslandRoots(c, root);
    std::sort(got.begin(), got.end());
    const std::vector<gate_t> want = frontierByDefinition(c, root);
    assert(got == want);
    if(!want.empty())
      ++nonempty;
  }
  assert(nonempty * 5 > trials);
}

static void testFrontierChildrenOnlyExample() {
  BooleanCircuit c;
  gate_t x = c.setGate("x", BooleanGate::IN, 0.5);
  gate_t b = c.setGate(BooleanGate::NOT);
  c.addWire(b, x);
  gate_t v = c.setGate(BooleanGate::NOT);
  c.addWire(v, b);
  gate_t w = c.setGate(BooleanGate::NOT);
  c.addWire(w, x);
  gate_t r = c.setGate(BooleanGate::OR);
  c.addWire(r, v);
  c.addWire(r, w);
  assert(maximalIslandRoots(c, r).empty());
  assert(frontierByDefinition(c, r).empty());
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

  ProbCache::Entry first;
  first.dd = std::move(target);
  ProbCache::Entry second = std::move(first);
  assert(second.dd.getNbGates() == 3);
  assert(first.dd.getNbGates() == 0);
}

static void testAdmits() {
  ProbCache::instance().reset();
  ProbCache &cache = ProbCache::instance();
  assert(cache.admits(1u << 20));
  assert(cache.stats().rejections == 0);
  assert(!cache.admits(size_t{1} << 40));
  assert(cache.stats().rejections == 1);
  ProbCache::instance().reset();
}

static double exactProbability(const BooleanCircuit &c, gate_t root) {
  std::vector<gate_t> inputs;
  for(gate_t g{0}; g < c.getNbGates(); ++g)
    if(c.getGateType(g) == BooleanGate::IN)
      inputs.push_back(g);
  double total = 0.;
  for(size_t world = 0; world < (size_t{1} << inputs.size()); ++world) {
    std::vector<int> value(c.getNbGates(), -1);
    double weight = 1.;
    for(size_t i = 0; i < inputs.size(); ++i) {
      const bool on = (world >> i) & 1;
      value[static_cast<size_t>(inputs[i])] = on;
      weight *= on ? c.getProb(inputs[i]) : 1. - c.getProb(inputs[i]);
    }
    std::function<bool(gate_t)> eval = [&](gate_t g) {
      int &v = value[static_cast<size_t>(g)];
      if(v >= 0)
        return v == 1;
      bool r = c.getGateType(g) == BooleanGate::AND;
      for(gate_t w : c.getWires(g)) {
        const bool x = eval(w);
        if(c.getGateType(g) == BooleanGate::AND)
          r = r && x;
        else if(c.getGateType(g) == BooleanGate::OR)
          r = r || x;
        else
          r = !x;
      }
      v = r;
      return r;
    };
    if(eval(root))
      total += weight;
  }
  return total;
}

static void testFolding() {
  size_t compiles = 0;
  ProbCacheCompileFn compile = [&compiles](const BooleanCircuit &cc, gate_t g) {
    ++compiles;
    TreeDecomposition td(cc);
    return dDNNFTreeDecompositionBuilder{cc, g, td}.build();
  };
  const ProbCacheOptions fold{true, true};
  const ProbCacheOptions fold_only{false, true};
  ProbCache &cache = ProbCache::instance();

  for(BooleanGate type : {BooleanGate::OR, BooleanGate::AND}) {
    BooleanCircuit c;
    gate_t root = c.setGate(type);
    double product = 1.;
    for(int i = 0; i < 5000; ++i) {
      const double p = 0.1 + 0.8 * (i % 7) / 7;
      c.addWire(root, c.setGate("flat" + std::to_string(i), BooleanGate::IN, p));
      product *= type == BooleanGate::AND ? p : 1. - p;
    }
    cache.reset();
    compiles = 0;
    bool ran = true;
    const double got = probCachedProbability(c, root, compile, &ran, fold);
    const double want = type == BooleanGate::AND ? product : 1. - product;
    assert(std::fabs(got - want) < 1e-12);
    assert(compiles == 0);
    assert(!ran);
    assert(cache.stats().hits + cache.stats().misses == 0);
  }

  {
    BooleanCircuit c;
    gate_t empty_and = c.setGate(BooleanGate::AND);
    gate_t empty_or = c.setGate(BooleanGate::OR);
    compiles = 0;
    assert(probCachedProbability(c, empty_and, compile, nullptr, fold) == 1.);
    assert(probCachedProbability(c, empty_or, compile, nullptr, fold) == 0.);
    assert(compiles == 0);
  }

  {
    BooleanCircuit c;
    gate_t x = c.setGate("dup_x", BooleanGate::IN, 0.3);
    gate_t y = c.setGate("dup_y", BooleanGate::IN, 0.4);
    gate_t root = c.setGate(BooleanGate::OR);
    c.addWire(root, x);
    c.addWire(root, x);
    c.addWire(root, y);
    compiles = 0;
    const double got = probCachedProbability(c, root, compile, nullptr, fold);
    assert(std::fabs(got - (1 - 0.7 * 0.6)) < 1e-12);
    assert(compiles == 0);
  }

  auto orders = [](const std::string &tag, gate_t &root) {
    BooleanCircuit c;
    const std::vector<std::vector<double> > shape = {{0.5, 0.2, 0.3}, {0.6, 0.7}, {0.8, 0.9, 0.4}};
    root = c.setGate(BooleanGate::OR);
    for(size_t o = 0; o < shape.size(); ++o) {
      gate_t order = c.setGate(tag + "o" + std::to_string(o), BooleanGate::IN, shape[o][0]);
      for(size_t l = 1; l < shape[o].size(); ++l) {
        gate_t line = c.setGate(tag + "l" + std::to_string(o) + "_" + std::to_string(l), BooleanGate::IN, shape[o][l]);
        gate_t pair = c.setGate(BooleanGate::AND);
        c.addWire(pair, order);
        c.addWire(pair, line);
        c.addWire(root, pair);
      }
    }
    return c;
  };

  {
    gate_t root;
    BooleanCircuit c = orders("a", root);
    const double want = exactProbability(c, root);

    cache.reset();
    compiles = 0;
    const double folded = probCachedProbability(c, root, compile, nullptr, fold);
    assert(std::fabs(folded - want) < 1e-12);
    assert(compiles == 1);
    assert(cache.stats().hits == 1);
    assert(cache.stats().misses == 1);

    cache.reset();
    compiles = 0;
    const double alone = probCachedProbability(c, root, compile, nullptr, fold_only);
    assert(std::fabs(alone - want) < 1e-12);
    assert(compiles == 2);
    assert(cache.stats().hits + cache.stats().misses == 0);
  }

  {
    BooleanCircuit c;
    std::vector<gate_t> v;
    for(int i = 0; i < 4; ++i)
      v.push_back(c.setGate("chain" + std::to_string(i), BooleanGate::IN, 0.2 + 0.2 * i));
    gate_t root = c.setGate(BooleanGate::OR);
    for(int i = 0; i < 3; ++i) {
      gate_t pair = c.setGate(BooleanGate::AND);
      c.addWire(pair, v[i]);
      c.addWire(pair, v[i + 1]);
      c.addWire(root, pair);
    }
    cache.reset();
    compiles = 0;
    const double got = probCachedProbability(c, root, compile, nullptr, fold);
    assert(std::fabs(got - exactProbability(c, root)) < 1e-12);
    assert(compiles == 1);
    assert(cache.stats().misses == 1);
  }

  {
    BooleanCircuit c;
    gate_t a = c.setGate("nest_a", BooleanGate::IN, 0.3);
    gate_t b = c.setGate("nest_b", BooleanGate::IN, 0.4);
    gate_t d = c.setGate("nest_d", BooleanGate::IN, 0.5);
    gate_t e = c.setGate("nest_e", BooleanGate::IN, 0.6);
    gate_t left = c.setGate(BooleanGate::OR);
    c.addWire(left, a);
    c.addWire(left, b);
    gate_t right = c.setGate(BooleanGate::OR);
    c.addWire(right, d);
    c.addWire(right, e);
    gate_t root = c.setGate(BooleanGate::AND);
    c.addWire(root, left);
    c.addWire(root, right);
    compiles = 0;
    const double got = probCachedProbability(c, root, compile, nullptr, fold);
    assert(std::fabs(got - (1 - 0.7 * 0.6) * (1 - 0.5 * 0.4)) < 1e-12);
    assert(compiles == 0);
  }

  {
    BooleanCircuit c;
    gate_t prev = c.setGate("deep0", BooleanGate::IN, 0.9);
    double want = 0.9;
    for(int i = 1; i < 2000; ++i) {
      gate_t x = c.setGate("deep" + std::to_string(i), BooleanGate::IN, 0.999);
      gate_t n = c.setGate(BooleanGate::AND);
      c.addWire(n, x);
      c.addWire(n, prev);
      prev = n;
      want *= 0.999;
    }
    const double got = probCachedProbability(c, prev, compile, nullptr, fold);
    assert(std::fabs(got - want) < 1e-9);
  }

  {
    std::mt19937 rng(20261102);
    int wrong_fold = 0, wrong_alone = 0;
    for(int round = 0; round < 400; ++round) {
      BooleanCircuit c;
      std::vector<gate_t> nodes;
      const int inputs = 2 + rng() % 8;
      for(int i = 0; i < inputs; ++i)
        nodes.push_back(c.setGate("r" + std::to_string(round) + "_" + std::to_string(i), BooleanGate::IN,
                                  0.1 + 0.8 * (rng() % 100) / 100.));
      const int gates = 1 + rng() % 8;
      for(int i = 0; i < gates; ++i) {
        const unsigned kind = rng() % 5;
        gate_t g = c.setGate(kind == 0 ? BooleanGate::NOT : kind % 2 ? BooleanGate::AND : BooleanGate::OR);
        const int fanin = kind == 0 ? 1 : 1 + rng() % 4;
        for(int k = 0; k < fanin; ++k)
          c.addWire(g, nodes[rng() % nodes.size()]);
        nodes.push_back(g);
      }
      gate_t root = c.setGate(rng() % 2 ? BooleanGate::AND : BooleanGate::OR);
      for(size_t k = 0; k < nodes.size(); ++k)
        if(rng() % 2)
          c.addWire(root, nodes[k]);
      c.addWire(root, nodes.back());
      const double want = exactProbability(c, root);
      if(std::fabs(probCachedProbability(c, root, compile, nullptr, fold) - want) > 1e-9)
        ++wrong_fold;
      if(std::fabs(probCachedProbability(c, root, compile, nullptr, fold_only) - want) > 1e-9)
        ++wrong_alone;
    }
    assert(wrong_fold == 0);
    assert(wrong_alone == 0);
  }
  cache.reset();
}

int main() {
  testFolding();
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
  testFrontierAgainstDefinitions();
  testFrontierChildrenOnlyExample();
  std::cout << "all tests passed" << std::endl;
  return 0;
}
