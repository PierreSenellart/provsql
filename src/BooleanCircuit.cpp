/**
 * @file BooleanCircuit.cpp
 * @brief Boolean circuit implementation and evaluation algorithms.
 *
 * Implements the methods declared in @c BooleanCircuit.h, including:
 * - Gate management (@c addGate, @c setGate, @c setInfo, @c setProb).
 * - Probability evaluation algorithms: possible worlds, Monte Carlo,
 *   WeightMC, independent evaluation.
 * - Knowledge compilation: @c compilation() (external tools),
 *   @c interpretAsDD() (direct from circuit structure),
 *   @c makeDD() (dispatcher).
 * - @c rewriteMultivaluedGates(): replaces MULVAR/MULIN clusters with
 *   standard AND/OR/NOT circuits.
 * - @c TseytinCNF(): DIMACS/weighted CNF generation for model counters.
 * - @c exportCircuit(): serialisation in the @c tdkc text format.
 * - @c toString(): human-readable gate description.
 *
 * In the standalone @c tdkc build (when @c TDKC is defined) a lightweight
 * @c elog() stub replaces the PostgreSQL error-reporting function.
 */
#include "BooleanCircuit.h"
#include "Circuit.hpp"
#include <type_traits>

extern "C" {
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>
}

#include <cassert>
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>
#include <stack>
#include <functional>
#include <algorithm>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

#include "dDNNFTreeDecompositionBuilder.h"
#include "external_tool.h"
// The tool registry drives external-tool selection and invocation, all of
// which lives in #ifndef TDKC blocks (tdkc invokes no external tool), so the
// registry is needed only in the extension build.
#ifndef TDKC
#include "ToolRegistry.h"
#include "kcmcp_client.h"
#endif

// "provsql_utils.h"
#ifdef TDKC
constexpr bool provsql_interrupted = false;
constexpr int provsql_verbose = 0;
constexpr int provsql_monte_carlo_seed = -1;
// makeDD's final fallback uses this GUC in the extension build; the
// standalone tdkc tool has no GUC layer, so default it to "d4".
constexpr const char *provsql_fallback_compiler = "d4";
enum levels {ERROR, NOTICE};
#define elog(level, ...) {fprintf(stderr, __VA_ARGS__); if(level==ERROR) exit(EXIT_FAILURE);}
#define CHECK_FOR_INTERRUPTS() ((void)0)
// The standalone tool has no PostgreSQL stack-depth governor; its deep
// recursions (tree decomposition, d-DNNF) already run on heap stacks.
#define check_stack_depth() ((void)0)
#else
extern "C" {
#include "provsql_utils.h"
#include "utils/elog.h"
#include "miscadmin.h"
}
#endif
#include "provsql_error.h"
#include "scoped_tempdir.h"
using provsql::ScopedTempDir;

namespace {

/**
 * @brief Best-effort parse of a model counter's "result line".
 *
 * Ganak, SharpSAT-TD, and DPMC each emit their final count on a
 * line like @c "c s exact arb float 0.4350..." or
 * @c "c s exact arb int 12345" (and DPMC also accepts the older
 * @c "s wmc N" shape). Historically we kept the right-most
 * whitespace-separated token and fed it to @c std::stod; that
 * relies on the value always being last on the line, which the
 * DPMC source itself notes is not stable across versions (a
 * trailing @c "(cputime ...)" suffix would silently corrupt the
 * parse).
 *
 * This helper scans tokens right-to-left and returns the
 * right-most one that parses as a @c double or as a
 * @c <num>/<den> rational. Throws a clear
 * @c "<tool>: could not parse '<line>'" instead of letting
 * @c std::stod's @c std::invalid_argument leak through.
 */
#ifndef TDKC  // external model-counter output parser; tdkc runs no counter
double parse_wmc_value(const std::string &line, const char *tool) {
  std::vector<std::string> tokens;
  std::stringstream ss(line);
  std::string tok;
  while(ss >> tok) tokens.push_back(tok);

  for(auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
    const std::string &t = *it;
    try {
      auto slash = t.find('/');
      if(slash != std::string::npos) {
        size_t pn = 0, pd = 0;
        double num = std::stod(t.substr(0, slash), &pn);
        double den = std::stod(t.substr(slash + 1), &pd);
        if(pn != slash || pd != t.size() - slash - 1) continue;
        return (den == 0.0) ? 0.0 : num / den;
      }
      size_t p = 0;
      double v = std::stod(t, &p);
      if(p == t.size()) return v;
    } catch(const std::exception &) {
      // Not a number; try the next token to the left.
    }
  }
  throw CircuitException(std::string(tool) + ": could not parse '" + line + "'");
}
#endif

}  // anonymous namespace

gate_t BooleanCircuit::setGate(BooleanGate type)
{
  auto id = Circuit::setGate(type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type)
{
  auto id = Circuit::setGate(u, type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type, double p)
{
  auto id = setGate(u, type);
  if(std::isnan(p))
    p=1.;
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::setGate(BooleanGate type, double p)
{
  auto id = setGate(type);
  if(std::isnan(p))
    p=1.;
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::addGate()
{
  auto id=Circuit::addGate();
  prob.push_back(1);
  return id;
}

std::string BooleanCircuit::toString(gate_t g) const
{
  return toStringHelper(g, BooleanGate::UNDETERMINED, nullptr);
}

std::string BooleanCircuit::toString(
  gate_t g,
  const std::unordered_map<gate_t, std::string> &labels) const
{
  return toStringHelper(g, BooleanGate::UNDETERMINED, &labels);
}

std::string BooleanCircuit::toStringHelper(
  gate_t g,
  BooleanGate parent,
  const std::unordered_map<gate_t, std::string> *labels) const
{
  std::string op;
  std::string result;
  auto gtype = getGateType(g);

  switch(gtype) {
  case BooleanGate::IN:
    if(labels) {
      auto it = labels->find(g);
      if(it != labels->end())
        return it->second;
    }
    return "x"+to_string(g);
  case BooleanGate::MULIN:
    if(labels) {
      auto it = labels->find(g);
      if(it != labels->end())
        return it->second + "[" + std::to_string(getProb(g)) + "]";
    }
    return "{" + to_string(*getWires(g).begin()) + "=" + std::to_string(getInfo(g)) + "}[" + std::to_string(getProb(g)) + "]";
  case BooleanGate::NOT:
    op="¬";
    break;
  case BooleanGate::UNDETERMINED:
    op="?";
    break;
  case BooleanGate::AND:
    op="∧";
    break;
  case BooleanGate::OR:
    op="∨";
    break;
  case BooleanGate::MULVAR:
    ;   // already dealt with in MULIN
  }

  if(getWires(g).empty()) {
    if(gtype==BooleanGate::AND)
      return "⊤";
    else if(gtype==BooleanGate::OR)
      return "⊥";
    else return op;
  }

  for(auto s: getWires(g)) {
    if(gtype==BooleanGate::NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toStringHelper(s, gtype, labels);
  }

  // Parenthesis elision:
  //   * single-wire AND/OR: the join carries no information, drop the wrap.
  //   * root call (parent = UNDETERMINED): no enclosing context, drop the wrap.
  //   * same-op nesting (parent == gtype, AND/OR only): associative, drop the wrap.
  bool single_join = (gtype==BooleanGate::AND || gtype==BooleanGate::OR)
                     && getWires(g).size()==1;
  bool same_op_assoc = (gtype==BooleanGate::AND || gtype==BooleanGate::OR)
                       && parent==gtype;
  if(single_join || parent==BooleanGate::UNDETERMINED || same_op_assoc)
    return result;
  return "("+result+")";
}

std::string BooleanCircuit::exportCircuit(gate_t root) const
{
  std::stringstream ss;

  std::unordered_set<gate_t> processed;
  std::stack<gate_t> to_process;
  to_process.push(root);

  while(!to_process.empty()) {
    auto g=to_process.top();
    to_process.pop();

    if(processed.find(g)!=processed.end())
      continue;

    ss << g << " ";

    switch(getGateType(g)) {
    case BooleanGate::IN:
      ss << "IN " << getProb(g);
      break;

    case BooleanGate::NOT:
      ss << "NOT " << getWires(g)[0];
      break;

    case BooleanGate::AND:
      ss << "AND";

      for(auto s:getWires(g))
        ss << " " << s;
      break;

    case BooleanGate::OR:
      ss << "OR";

      for(auto s:getWires(g))
        ss << " " << s;
      break;

    case BooleanGate::MULVAR:
    case BooleanGate::MULIN:
    case BooleanGate::UNDETERMINED:
      assert(false);   // not done
    }

    ss << "\n";

    for(auto s: getWires(g)) {
      if(processed.find(s)==processed.end())
        to_process.push(s);
    }

    processed.insert(g);
  }

  return ss.str();
}

bool BooleanCircuit::evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const
{
  check_stack_depth(); // recurses on wires; guard deep circuits (see GenericCircuit::evaluate)
  bool disjunction=false;

  switch(getGateType(g)) {
  case BooleanGate::IN:
    return sampled.find(g)!=sampled.end();
  case BooleanGate::MULIN:
  case BooleanGate::MULVAR:
    throw CircuitException("Monte-Carlo sampling not implemented on multivalued inputs");
  case BooleanGate::NOT:
    return !evaluate(*(getWires(g).begin()), sampled);
  case BooleanGate::AND:
    disjunction = false;
    break;
  case BooleanGate::OR:
    disjunction = true;
    break;
  case BooleanGate::UNDETERMINED:
    throw CircuitException("Incorrect gate type");
  }

  for(auto s: getWires(g)) {
    bool e = evaluate(s, sampled);
    if(disjunction && e)
      return true;
    if(!disjunction && !e)
      return false;
  }

  if(disjunction)
    return false;
  else
    return true;
}

double BooleanCircuit::monteCarlo(gate_t g, unsigned samples) const
{
  // Seed mt19937_64 from the provsql.monte_carlo_seed GUC: -1 (the
  // default) means non-deterministic via std::random_device, any other
  // value (including 0) is a literal seed so regression tests can pin
  // sampling for reproducibility.
  std::mt19937_64 rng;
  if(provsql_monte_carlo_seed != -1) {
    rng.seed(static_cast<uint64_t>(provsql_monte_carlo_seed));
  } else {
    std::random_device rd;
    rng.seed((static_cast<uint64_t>(rd()) << 32) | rd());
  }
  std::uniform_real_distribution<double> uniform01(0.0, 1.0);

  auto success{0u};

  for(unsigned i=0; i<samples; ++i) {
    std::unordered_set<gate_t> sampled;
    for(auto in: inputs) {
      if(uniform01(rng) < getProb(in)) {
        sampled.insert(in);
      }
    }

    if(evaluate(g, sampled))
      ++success;

    if(provsql_interrupted)
      throw CircuitException("Interrupted after "+std::to_string(i+1)+" samples");
  }

  return success*1./samples;
}

bool BooleanCircuit::dnfShape(
  gate_t g,
  std::vector<gate_t> &clauses,
  std::vector<std::set<gate_t> > &supports) const
{
  clauses.clear();
  supports.clear();

  // A top-level OR exposes one clause per child; anything else is a single
  // clause rooted at g itself (regime (a): a bare AND-of-leaves or a lone
  // input).
  std::vector<gate_t> clause_roots;
  if(getGateType(g)==BooleanGate::OR) {
    for(auto c: getWires(g))
      clause_roots.push_back(c);
  } else {
    clause_roots.push_back(g);
  }

  for(auto root: clause_roots) {
    // Sweep the AND-only stratum below this clause root, collecting the
    // reachable input leaves.  Any OR (nested disjunction), NOT
    // (negation), or multivalued input below the root takes the circuit
    // out of regimes (a)/(b): bail.
    std::set<gate_t> support;
    std::unordered_set<gate_t> seen;
    std::stack<gate_t> st;
    st.push(root);
    while(!st.empty()) {
      gate_t cur = st.top();
      st.pop();
      if(!seen.insert(cur).second)
        continue;
      switch(getGateType(cur)) {
      case BooleanGate::IN:
        support.insert(cur);
        break;
      case BooleanGate::AND:
        for(auto s: getWires(cur))
          st.push(s);
        break;
      default:
        return false;
      }
    }
    clauses.push_back(root);
    supports.push_back(std::move(support));
  }

  return true;
}

bool BooleanCircuit::dnfShapeInfo(gate_t g, std::size_t &num_clauses) const
{
  // Clause count: children of a top-level OR, else a single clause rooted at g.
  std::vector<gate_t> clause_roots;
  if(getGateType(g)==BooleanGate::OR)
    for(auto c: getWires(g))
      clause_roots.push_back(c);
  else
    clause_roots.push_back(g);
  num_clauses = clause_roots.size();

  // Validate the AND-only strata below every clause root with ONE global
  // visited-set (each gate's type is path-independent), so a shared subgraph is
  // checked once and no per-clause supports are materialised.  O(circuit).
  std::unordered_set<gate_t> seen;
  std::stack<gate_t> st;
  for(auto r: clause_roots)
    st.push(r);
  while(!st.empty()) {
    gate_t cur = st.top();
    st.pop();
    if(!seen.insert(cur).second)
      continue;
    switch(getGateType(cur)) {
    case BooleanGate::IN:
      break;
    case BooleanGate::AND:
      for(auto s: getWires(cur))
        st.push(s);
      break;
    default:
      return false;
    }
  }
  return true;
}

namespace {

/**
 * Shared Karp-Luby sampler state derived from the per-clause supports:
 * the per-clause probability @c p_i = product of its support-leaf marginals,
 * the prefix sums for the O(log m) categorical clause draw, the union-bound
 * total @c S = sum p_i (with @c Pr[F] <= S <= m*Pr[F]), and the set of leaves
 * that can affect clause membership (only those need to be drawn each round).
 */
struct KarpLubyState {
  std::vector<double> p;
  std::vector<double> cumulative;
  double S = 0.;
  std::vector<gate_t> relevant;
};

KarpLubyState karpLubyState(
  const BooleanCircuit &c,
  const std::vector<std::set<gate_t> > &supports)
{
  KarpLubyState st;
  const size_t m = supports.size();
  st.p.resize(m);
  st.cumulative.resize(m);
  std::set<gate_t> rel;
  for(size_t i=0; i<m; ++i) {
    double pi = 1.;
    for(gate_t leaf: supports[i]) {
      pi *= c.getProb(leaf);
      rel.insert(leaf);
    }
    st.p[i] = pi;
    st.S += pi;
    st.cumulative[i] = st.S;
  }
  st.relevant.assign(rel.begin(), rel.end());
  return st;
}

/// Seed mt19937_64 from provsql.monte_carlo_seed exactly as monteCarlo, so a
/// pinned seed makes the estimate reproducible for the regression tests.
std::mt19937_64 karpLubySeededRNG()
{
  std::mt19937_64 rng;
  if(provsql_monte_carlo_seed != -1) {
    rng.seed(static_cast<uint64_t>(provsql_monte_carlo_seed));
  } else {
    std::random_device rd;
    rng.seed((static_cast<uint64_t>(rd()) << 32) | rd());
  }
  return rng;
}

/// Draw a clause index with probability @c p_i / S using the prefix sums.
size_t karpLubyDrawClause(const KarpLubyState &st,
                          std::mt19937_64 &rng,
                          std::uniform_real_distribution<double> &u01)
{
  double u = u01(rng) * st.S;
  size_t i = static_cast<size_t>(
    std::upper_bound(st.cumulative.begin(), st.cumulative.end(), u)
    - st.cumulative.begin());
  if(i >= st.cumulative.size())
    i = st.cumulative.size() - 1;   // guard against u == S from rounding
  return i;
}

/**
 * One Karp-Luby coverage trial in clause @p i: sample an assignment of
 * @c C_i (its support forced true, every other relevant leaf drawn from its
 * marginal), then return whether @p i is the smallest-index clause the
 * assignment satisfies -- the coverage rejection that divides the over-count
 * @c S by the number of clauses covering each sampled world.  @p trueLeaves is
 * reused across calls to avoid reallocating.
 */
bool karpLubyCovers(
  const BooleanCircuit &c,
  const std::vector<std::set<gate_t> > &supports,
  const KarpLubyState &st, size_t i,
  std::mt19937_64 &rng,
  std::uniform_real_distribution<double> &u01,
  std::unordered_set<gate_t> &trueLeaves)
{
  trueLeaves.clear();
  for(gate_t leaf: st.relevant) {
    if(supports[i].count(leaf) || u01(rng) < c.getProb(leaf))
      trueLeaves.insert(leaf);
  }
  const size_t m = supports.size();
  for(size_t j=0; j<m; ++j) {
    bool sat = true;
    for(gate_t leaf: supports[j]) {
      if(trueLeaves.find(leaf)==trueLeaves.end()) { sat = false; break; }
    }
    if(sat)
      return j==i;
  }
  return false;   // unreachable: clause i always covers its own forced support
}

} // anonymous namespace

double BooleanCircuit::karpLuby(
  const std::vector<gate_t> &clauses,
  const std::vector<std::set<gate_t> > &supports,
  unsigned long samples) const
{
  const size_t m = clauses.size();
  if(m==0 || samples==0)
    return 0.;

  KarpLubyState st = karpLubyState(*this, supports);
  if(st.S<=0.)
    return 0.;

  std::mt19937_64 rng = karpLubySeededRNG();
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  std::unordered_set<gate_t> trueLeaves;

  // Fewer rounds than clauses: too few to stratify (every clause needs at
  // least one sample for its per-clause acceptance rate to be defined), so
  // fall back to the unstratified categorical-draw estimator -- S times the
  // overall acceptance ratio, still unbiased for any budget.
  if(samples < m) {
    unsigned long accepts = 0;
    for(unsigned long s=0; s<samples; ++s) {
      size_t i = karpLubyDrawClause(st, rng, u01);
      if(karpLubyCovers(*this, supports, st, i, rng, u01, trueLeaves))
        ++accepts;
      if(provsql_interrupted)
        throw CircuitException("Interrupted after "+std::to_string(s+1)+" samples");
    }
    return st.S * accepts / static_cast<double>(samples);
  }

  // Stratified allocation: n_i = 1 + proportional share of (samples - m) by
  // p_i / S, with the leftover rounds handed to the largest fractional parts
  // (largest-remainder rounding) so the n_i stay proportional and sum to
  // exactly `samples`.  Estimating each clause's acceptance rate separately
  // and combining sum_i p_i * rate_i removes the categorical-draw
  // (between-strata) variance of the textbook estimator, tightening the
  // estimate at the same budget by up to a factor m.
  std::vector<unsigned long> n(m, 1);
  const unsigned long rest = samples - m;
  std::vector<double> frac(m);
  unsigned long base_sum = 0;
  for(size_t i=0; i<m; ++i) {
    double want = static_cast<double>(rest) * st.p[i] / st.S;
    unsigned long fl = static_cast<unsigned long>(want);
    n[i] += fl;
    base_sum += fl;
    frac[i] = want - static_cast<double>(fl);
  }
  unsigned long leftover = rest - base_sum;
  if(leftover > 0) {
    std::vector<size_t> idx(m);
    for(size_t i=0; i<m; ++i) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin()+leftover, idx.end(),
      [&](size_t a, size_t b){ return frac[a] > frac[b]; });
    for(unsigned long k=0; k<leftover; ++k)
      ++n[idx[k]];
  }

  double est = 0.;
  for(size_t i=0; i<m; ++i) {
    unsigned long accepts = 0;
    for(unsigned long k=0; k<n[i]; ++k) {
      if(karpLubyCovers(*this, supports, st, i, rng, u01, trueLeaves))
        ++accepts;
      if(provsql_interrupted)
        throw CircuitException("Interrupted while sampling clause "
                               +std::to_string(i));
    }
    est += st.p[i] * static_cast<double>(accepts) / static_cast<double>(n[i]);
  }
  return est;
}

double BooleanCircuit::karpLubyStopping(
  const std::vector<gate_t> &clauses,
  const std::vector<std::set<gate_t> > &supports,
  double eps, double delta,
  unsigned long max_samples,
  unsigned long &samples_used,
  bool &reached_target) const
{
  samples_used = 0;
  reached_target = false;
  const size_t m = clauses.size();
  if(m==0 || max_samples==0)
    return 0.;

  KarpLubyState st = karpLubyState(*this, supports);
  if(st.S<=0.)
    return 0.;

  // DKLR stopping threshold on the accept count: Y1 = 1 + (1+eps)*Y with
  // Y = 4*(e-2)*ln(2/delta)/eps^2.  Sample coverage trials until the accept
  // count reaches Y1 and return S*Y1/N (a relative (eps,delta) estimate of
  // Pr[F]); the number of rounds N then adapts to the true acceptance
  // probability Pr[F]/S in [1/m, 1] -- up to m times fewer rounds than the
  // fixed bound when the clauses barely overlap.
  const double e  = exp(1.0);
  const double Y  = 4.0 * (e - 2.0) * log(2.0/delta) / (eps*eps);
  const double Y1 = 1.0 + (1.0 + eps) * Y;

  std::mt19937_64 rng = karpLubySeededRNG();
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  std::unordered_set<gate_t> trueLeaves;

  unsigned long accepts = 0;
  for(unsigned long s=0; s<max_samples; ++s) {
    size_t i = karpLubyDrawClause(st, rng, u01);
    if(karpLubyCovers(*this, supports, st, i, rng, u01, trueLeaves)) {
      ++accepts;
      if(static_cast<double>(accepts) >= Y1) {
        samples_used = s + 1;
        reached_target = true;
        return st.S * Y1 / static_cast<double>(samples_used);
      }
    }
    if(provsql_interrupted)
      throw CircuitException("Interrupted after "+std::to_string(s+1)+" samples");
  }

  // Cap reached before the threshold: the (eps,delta) target is not met, so
  // return the plain unbiased S*accepts/N estimate over the spent budget (the
  // caller reports the weaker guarantee actually achieved).
  samples_used = max_samples;
  return st.S * static_cast<double>(accepts) / static_cast<double>(max_samples);
}

/// Largest clause count for which the 2^m sieve enumeration is admitted.
static const size_t kSieveMaxClauses = 24;

double BooleanCircuit::sieve(
  const std::vector<gate_t> &clauses,
  const std::vector<std::set<gate_t> > &supports) const
{
  const size_t m = clauses.size();
  if(m == 0)
    return 0.;
  if(m > kSieveMaxClauses)
    throw CircuitException(
      "sieve: too many clauses (" + std::to_string(m) + " > "
      + std::to_string(kSieveMaxClauses)
      + "); inclusion-exclusion is 2^m -- use another method");

  // Pr[∨ c_i] = Σ_{∅≠S} (-1)^{|S|+1} ∏_{leaf ∈ ∪supports(S)} getProb(leaf).
  double total = 0.;
  std::unordered_set<gate_t> u;
  for(unsigned long long s = 1; s < (1ULL << m); ++s) {
    u.clear();
    int bits = 0;
    for(size_t i = 0; i < m; ++i)
      if(s & (1ULL << i)) {
        ++bits;
        for(gate_t leaf : supports[i])
          u.insert(leaf);
      }
    double p = 1.;
    for(gate_t leaf : u)
      p *= getProb(leaf);
    if(bits & 1) total += p; else total -= p;

    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }
  return total;
}

void BooleanCircuit::dnfBounds(
  const std::vector<std::set<gate_t> > &clauses,
  double &lower, double &upper) const
{
  const size_t m = clauses.size();
  if(m == 0) {
    lower = upper = 0.;
    return;
  }

  // Per-clause probability P(d) = ∏_{leaf ∈ clauses[d]} getProb(leaf) (an empty
  // support is a constant-true clause, product over the empty set = 1).
  std::vector<double> clause_prob(m);
  for(size_t i = 0; i < m; ++i) {
    double p = 1.;
    for(gate_t leaf : clauses[i])
      p *= getProb(leaf);
    clause_prob[i] = p;
  }

  // Greedy partition into buckets of pairwise-independent clauses, clauses taken
  // in descending marginal-probability order (the paper's improved heuristic).
  std::vector<size_t> order(m);
  for(size_t i = 0; i < m; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) {
              return clause_prob[a] > clause_prob[b];
            });

  // For each bucket: the union of its clauses' supports (to test independence in
  // O(|support|) against the whole bucket at once -- disjoint from the union iff
  // independent of every clause already in it) and its running independent-or
  // probability 1 - ∏(1 - P(d)).
  std::vector<std::set<gate_t> > bucket_support;
  std::vector<double> bucket_prob;
  for(size_t idx : order) {
    const std::set<gate_t> &sup = clauses[idx];
    size_t target = bucket_support.size();   // default: open a new bucket
    for(size_t b = 0; b < bucket_support.size(); ++b) {
      bool disjoint = true;
      for(gate_t leaf : sup)
        if(bucket_support[b].count(leaf)) {
          disjoint = false;
          break;
        }
      if(disjoint) {
        target = b;
        break;
      }
    }
    if(target == bucket_support.size()) {
      bucket_support.emplace_back();
      bucket_prob.push_back(0.);
    }
    bucket_prob[target] =
      1. - (1. - bucket_prob[target]) * (1. - clause_prob[idx]);
    bucket_support[target].insert(sup.begin(), sup.end());

    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  // lower = max bucket probability (each bucket is a sub-disjunction of Φ);
  // upper = min(1, Σ bucket probabilities) (union bound over the buckets).
  double L = 0., U = 0.;
  for(double bp : bucket_prob) {
    if(bp > L)
      L = bp;
    U += bp;
  }
  lower = L;
  upper = (U > 1.) ? 1. : U;
}

double BooleanCircuit::possibleWorlds(gate_t g) const
{
  if(inputs.size()>=8*sizeof(unsigned long long))
    throw CircuitException("Too many possible worlds to iterate over");

  unsigned long long nb=(1ULL<<inputs.size());
  double totalp=0.;

  for(unsigned long long i=0; i < nb; ++i) {
    std::unordered_set<gate_t> s;
    double p = 1;

    unsigned j=0;
    for(gate_t in : inputs) {
      if(i & (1ULL << j)) {
        s.insert(in);
        p*=getProb(in);
      } else {
        p*=1-getProb(in);
      }
      ++j;
    }

    if(evaluate(g, s))
      totalp+=p;

    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  return totalp;
}

std::string BooleanCircuit::TseytinCNF(gate_t g, bool display_prob, bool mapping) const {
  std::vector<std::vector<int> > clauses;

  // Tseytin transformation
  for(gate_t i{0}; i<gates.size(); ++i) {
    switch(getGateType(i)) {
    case BooleanGate::AND:
    {
      int id{static_cast<int>(i)+1};
      std::vector<int> c = {id};
      for(auto s: getWires(i)) {
        clauses.push_back({-id, static_cast<int>(s)+1});
        c.push_back(-static_cast<int>(s)-1);
      }
      clauses.push_back(c);
      break;
    }

    case BooleanGate::OR:
    {
      int id{static_cast<int>(i)+1};
      std::vector<int> c = {-id};
      for(auto s: getWires(i)) {
        clauses.push_back({id, -static_cast<int>(s)-1});
        c.push_back(static_cast<int>(s)+1);
      }
      clauses.push_back(c);
    }
    break;

    case BooleanGate::NOT:
    {
      int id=static_cast<int>(i)+1;
      auto s=*getWires(i).begin();
      clauses.push_back({-id,-static_cast<int>(s)-1});
      clauses.push_back({id,static_cast<int>(s)+1});
      break;
    }

    case BooleanGate::MULIN:
      throw CircuitException("Multivalued inputs should have been removed by then.");
    case BooleanGate::MULVAR:
    case BooleanGate::IN:
    case BooleanGate::UNDETERMINED:
      ;
    }
  }
  clauses.push_back({(int)g+1});

  std::ostringstream oss;
  // Optional self-documenting mapping, emitted as DIMACS comments
  // before the problem line so a saved CNF records which provenance
  // input each variable stands for. Comments are ignored by every
  // model counter / compiler, so the file stays valid DIMACS.
  if(mapping) {
    for(const auto &m : tseytinVariableMapping()) {
      oss << "c input " << m.variable << " "
          << (m.uuid.empty() ? "?" : m.uuid) << " "
          << m.probability << "\n";
    }
  }
  oss << "p cnf " << gates.size() << " " << clauses.size() << "\n";
  for(unsigned i=0; i<clauses.size(); ++i) {
    for(int x : clauses[i]) {
      oss << x << " ";
    }
    oss << "0\n";
  }
  if(display_prob) {
    for(gate_t in: inputs) {
      oss << "w " << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << getProb(in) << "\n";
      oss << "w -" << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << (1. - getProb(in)) << "\n";
    }
  }
  return oss.str();
}

std::vector<BooleanCircuit::CNFInputMapping>
BooleanCircuit::tseytinVariableMapping() const {
  std::vector<CNFInputMapping> mapping;
  // `inputs` is a std::set<gate_t>, so iteration is in gate-id order
  // and the variable indices (id + 1) come out sorted and stable.
  for(gate_t in : inputs) {
    auto id = static_cast<std::underlying_type<gate_t>::type>(in);
    std::string u;
    auto it = id2uuid.find(in);
    if(it != id2uuid.end())
      u = it->second;
    mapping.push_back({static_cast<int>(id) + 1, u, getProb(in)});
  }
  return mapping;
}

std::string BooleanCircuit::BCS12(gate_t g, std::vector<gate_t> &inputOrder) const {
  inputOrder.clear();
  auto idOf = [](gate_t x) {
    return static_cast<std::underlying_type<gate_t>::type>(x);
  };

  std::set<gate_t> seenInputs;
  std::set<gate_t> internalGates;   // AND/OR gates to emit, ordered by id

  // Resolve a wire to a BC-S1.2 literal, inlining NOT chains as sign flips.
  std::function<std::string(gate_t)> lit = [&](gate_t w) -> std::string {
    switch(getGateType(w)) {
    case BooleanGate::IN:
      return "in" + std::to_string(idOf(w));
    case BooleanGate::AND:
    case BooleanGate::OR:
      return "g" + std::to_string(idOf(w));
    case BooleanGate::NOT: {
      std::string inner = lit(*getWires(w).begin());
      return inner[0]=='-' ? inner.substr(1) : "-"+inner;
    }
    default:
      throw CircuitException("BC-S1.2 export: unsupported gate type");
    }
  };

  // DFS collecting input gates (in first-seen order, fixing their d4
  // variable numbers) and the AND/OR gates to define; NOT gates are
  // traversed but never named.
  std::function<void(gate_t)> collect = [&](gate_t w) {
    switch(getGateType(w)) {
    case BooleanGate::IN:
      if(seenInputs.insert(w).second)
        inputOrder.push_back(w);
      break;
    case BooleanGate::NOT:
      collect(*getWires(w).begin());
      break;
    case BooleanGate::AND:
    case BooleanGate::OR:
      if(internalGates.insert(w).second)
        for(gate_t c : getWires(w))
          collect(c);
      break;
    default:
      throw CircuitException("BC-S1.2 export: unsupported gate type");
    }
  };
  collect(g);

  std::ostringstream oss;
  oss << "c BC-S1.2\n";
  // Inputs first: d4 numbers them 1..k in this order (see header doc).
  for(gate_t in : inputOrder)
    oss << "I in" << idOf(in) << "\n";
  for(gate_t w : internalGates) {
    const auto &ch = getWires(w);
    if(ch.empty())
      throw CircuitException("BC-S1.2 export: nullary gate");
    oss << "G g" << idOf(w) << " := ";
    // BC-S1.2 requires >= 2 literals for A/O; a unary AND/OR is the identity.
    if(ch.size()==1)
      oss << "I";
    else
      oss << (getGateType(w)==BooleanGate::AND ? "A" : "O");
    for(gate_t c : ch)
      oss << " " << lit(c);
    oss << "\n";
  }
  oss << "T " << lit(g) << "\n";
  return oss.str();
}

// ---------------------------------------------------------------------------
// External-tool knowledge compilation and weighted model counting.
//
// The standalone tdkc tool deliberately invokes NO external tool (it compiles
// purely via tree decomposition), so this whole block -- the knowledge
// compilers, the Panini wrapper, the weighted model counters, and the
// makeDD/makeDDByName dispatchers that fall back to them -- is excluded from
// the tdkc build.  The registry is therefore used unconditionally here.
// ---------------------------------------------------------------------------
#ifndef TDKC

// Parse a Panini (KCBox) DD output file into a d-DNNF: this is the
// `panini-dd` output parser, selected by compilation() for the panini-*
// records (which run the generic compile path -- write a Tseytin CNF, run the
// record's argtpl -- and differ only in this parse-back).  Panini emits its
// own DD format (sequential node ids; "F"/"T" terminals; "C"/"D" decomposable
// conjunctions; decision nodes), not the c2d/d4 NNF the `nnf` parser reads.
// R2-D2 and CCDD emit "K" (kernelize) nodes that break decomposability, so
// ProvSQL does not register the variants that produce them.
dDNNF BooleanCircuit::parsePaniniDD(const std::string &outfilename) const {
  std::ifstream ifs(outfilename.c_str());
  if (!ifs)
    throw CircuitException("Cannot open Panini output: " + outfilename);

  // Skip Panini's preamble ("Variable order: ...", "Maximum variable: ...",
  // "Number of nodes: ...") and stop at the first data line, which always
  // starts with "0:".
  std::string line;
  bool found_data = false;
  while (std::getline(ifs, line)) {
    if (line.rfind("0:", 0) == 0) { found_data = true; break; }
  }
  if (!found_data)
    throw CircuitException("Panini output: no data lines found");

  dDNNF dnnf;
  // Panini node ids are sequential 0, 1, 2, ... Highest id is the
  // root of the compilation.
  std::vector<gate_t> id_to_gate;

  do {
    if (line.empty()) continue;
    auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) continue;

    // Sanity-check the leading id matches the size of id_to_gate so far
    // (the file should be in monotonically increasing id order).
    int panini_id = std::stoi(line.substr(0, colon_pos));
    if (static_cast<size_t>(panini_id) != id_to_gate.size())
      throw CircuitException(
              "Panini output: out-of-order node id "
              + std::to_string(panini_id));

    std::stringstream ss(line.substr(colon_pos + 1));
    std::string first;
    ss >> first;

    gate_t this_gate;
    if (first == "F") {
      // FALSE terminal: empty OR.
      this_gate = dnnf.setGate(BooleanGate::OR);
    } else if (first == "T") {
      // TRUE terminal: empty AND.
      this_gate = dnnf.setGate(BooleanGate::AND);
    } else if (first == "C" || first == "D") {
      // C (CONJOIN), D (DECOMPOSE) are decomposable conjunctions in
      // Panini's CDD format; OR is only ever expressed implicitly by
      // the (v ? t : f) decision nodes. K (KERNELIZE) nodes encode
      // literal-equivalence constraints over a shared kernel
      // variable and break decomposability; we refuse the only two
      // target languages that emit them (R2-D2 and CCDD) upstream,
      // so seeing K here is an upstream-Panini surprise.
      this_gate = dnnf.setGate(BooleanGate::AND);
      int child;
      while (ss >> child) {
        if (child == 0) break;
        if (child < 0 || static_cast<size_t>(child) >= id_to_gate.size())
          throw CircuitException(
                  "Panini output: forward / invalid child reference "
                  + std::to_string(child));
        dnnf.addWire(this_gate, id_to_gate[child]);
      }
    } else if (first == "K") {
      throw CircuitException(
              "Panini output: unexpected K (kernelize) node; ProvSQL "
              "does not support Panini target languages that emit K "
              "nodes (R2-D2, CCDD).");
    } else {
      // Decision node: <var> <false_child> <true_child> [0]
      // (Panini's CDD::Display emits children in ch[0]/ch[1] order;
      // CDD.cpp's DOT writer maps ch[0] to the dotted/false edge and
      // ch[1] to the solid/true edge.)
      int var = std::stoi(first);
      int f_child, t_child;
      if (!(ss >> f_child >> t_child))
        throw CircuitException(
                "Panini output: malformed decision line at id "
                + std::to_string(panini_id));
      if (t_child < 0 || f_child < 0
          || static_cast<size_t>(t_child) >= id_to_gate.size()
          || static_cast<size_t>(f_child) >= id_to_gate.size())
        throw CircuitException(
                "Panini output: forward / invalid decision child at id "
                + std::to_string(panini_id));
      gate_t t_gate = id_to_gate[t_child];
      gate_t f_gate = id_to_gate[f_child];

      // Translate the decision. Two cases:
      //   (a) v is an input gate: keep the literal in the structure,
      //       OR(AND(v, t'), AND(NOT(v), f')).
      //   (b) v is a Tseytin auxiliary: aux vars are functionally
      //       determined by inputs, so under WMC we want literal
      //       weights w(v) = w(NOT v) = 1 (not (p, 1-p)). With those
      //       weights the AND wrappers contribute 1 to either branch
      //       and we can drop them entirely, emitting just
      //       OR(t', f'). The OR is not determinism-preserving on
      //       the variable v, but the input-projection of its two
      //       arms is still disjoint by Tseytin determinism so
      //       @c dDNNF::probabilityEvaluation() still returns the
      //       correct weighted model count.
      size_t var_idx = static_cast<size_t>(var) - 1;
      if (var_idx < gates.size() && gates[var_idx] == BooleanGate::IN) {
        gate_t pos_lit = dnnf.setGate(
            getUUID(static_cast<gate_t>(var_idx)),
            BooleanGate::IN, prob[var_idx]);
        gate_t neg_lit = dnnf.setGate(BooleanGate::NOT);
        dnnf.addWire(neg_lit, pos_lit);
        gate_t and_t = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(and_t, pos_lit);
        dnnf.addWire(and_t, t_gate);
        gate_t and_f = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(and_f, neg_lit);
        dnnf.addWire(and_f, f_gate);
        this_gate = dnnf.setGate(BooleanGate::OR);
        dnnf.addWire(this_gate, and_t);
        dnnf.addWire(this_gate, and_f);
      } else {
        this_gate = dnnf.setGate(BooleanGate::OR);
        dnnf.addWire(this_gate, t_gate);
        dnnf.addWire(this_gate, f_gate);
      }
    }
    id_to_gate.push_back(this_gate);
  } while (std::getline(ifs, line));

  ifs.close();

  if (id_to_gate.empty())
    throw CircuitException("Panini output produced no nodes");

  // The root of a Panini DD is the highest-id node.
  dnnf.setRoot(id_to_gate.back());

  dnnf.simplify();
  return dnnf;
}

// Preference-ranked tool selection for @p operation: honour the explicitly
// @p preferred tool when it is enabled, advertises the operation, and is
// available; otherwise return the highest-preference enabled tool advertising
// the operation whose binary and dependencies resolve on PATH.  Returns "" if
// none is available, so a dispatcher can fall back or raise a clear error.
// This replaces the old "if d4 else c2d ..." chains: an admin reorders or
// disables tools (or bumps provsql.fallback_compiler, honoured via @p
// preferred) and selection follows.
static std::string selectTool(const std::string &operation,
                              const std::string &preferred = "") {
  if(!preferred.empty()) {
    const provsql::ToolRecord *r = provsql::tool_registry().find(preferred);
    if(r != nullptr && r->enabled && r->hasOperation(operation)
       && toolAvailable(*r))
      return preferred;
  }
  for(const provsql::ToolRecord *r : provsql::tool_registry().byOperation(operation))
    if(toolAvailable(*r))
      return r->name;
  return "";
}

// The compiler for makeDD's last-resort fallback route: prefer
// provsql.fallback_compiler (default "d4") when available, otherwise the
// highest-preference compiler whose binary resolves on PATH.  Falls back to
// the configured name when nothing is available, so compilation() raises its
// actionable error.  (The GUC governs only this fallback route; an explicit
// no-argument compilation() request just takes the best available compiler --
// symmetric with the no-tool wmc path.)
static std::string chooseCompiler() {
  const char *fb = (provsql_fallback_compiler != NULL
                    && provsql_fallback_compiler[0] != '\0')
                   ? provsql_fallback_compiler : "d4";
  std::string chosen = selectTool("compile", fb);
  return chosen.empty() ? std::string(fb) : chosen;
}

dDNNF BooleanCircuit::compilation(gate_t g, std::string compiler,
                                  std::string *resolved) const {
  // No compiler named: pick the highest-preference available one (symmetric
  // with the no-tool wmc path).  provsql.fallback_compiler is deliberately
  // not consulted here -- it governs only makeDD's last-resort fallback route.
  if(compiler.empty()) {
    compiler = selectTool("compile");
    if(compiler.empty())
      throw CircuitException(
              "no knowledge compiler is available; install one (d4, d4v2, "
              "c2d, minic2d, dsharp) or add its directory to "
              "provsql.tool_search_path");
  }

  // Validate the compiler against the registry before any temp-dir or CNF
  // work.  A name that is not a registered, enabled 'compile' tool is
  // rejected here.  compilation() implements two output parsers: the tolerant
  // `nnf` reader (both the d4-family header-less NNF and the c2d-style header
  // form) and the `panini-dd` reader (Panini's own DD format); a compile tool
  // advertising any other parser is something we cannot read back, so reject
  // it rather than mis-parse.
  const provsql::ToolRecord *rec = provsql::tool_registry().find(compiler);
  if(rec == nullptr || !rec->hasOperation("compile"))
    throw CircuitException("Unknown compiler '"+compiler+"'");
  if(!rec->enabled)
    throw CircuitException(
            "Compiler '"+compiler+"' is disabled in the tool registry");
  if(rec->parser != "nnf" && rec->parser != "panini-dd")
    throw CircuitException(
            "Compiler '"+compiler+"' uses output parser '"+rec->parser
            +"', which compilation() does not implement");
  const std::string compiler_binary = rec->binary;
  if(resolved)
    *resolved = compiler; // the validated tool actually used (CLI or KCMCP)

  // KCMCP backend: compile over a warm socket server instead of spawning a
  // CLI tool.  The problem is sent as a native BC-S1.2 circuit when the record
  // advertises that input, else as a Tseytin CNF; the RESULT's d-DNNF text is
  // parsed by the same parseDDNNF() the CLI path uses.  Any failure (connect,
  // protocol, server ERROR) raises, so makeDD's fallback can try another tool.
  if(rec->kind == "kcmcp") {
    std::vector<gate_t> inputOrder;
    std::string content;
    uint8_t input_format = 0;  // dimacs-cnf
    if(rec->acceptsInput("circuit-bcs12")) {
      try {
        content = BCS12(g, inputOrder);
        input_format = 1;  // circuit-bcs12
      } catch(const CircuitException &) {
        inputOrder.clear();
        content.clear();
      }
    }
    if(content.empty())
      content = TseytinCNF(g, false);  // inputOrder stays empty => CNF mode
    // Resolve the server address: a literal 'managed' endpoint defers to the
    // live address the supervisor worker published in shared memory; anything
    // else is a fixed endpoint (unix:/path or host:port).
    std::string endpoint = rec->endpoint;
    if(endpoint == "managed")
      endpoint = provsql_kcmcp_managed_endpoint();
    if(endpoint.empty())
      throw CircuitException(
              "KCMCP tool '"+compiler+"' has no endpoint (managed server not "
              "running, or provsql.kcmcp_server unset)");
    try {
      std::string nnf = provsql::kcmcp_compile(endpoint, input_format, content);
      std::istringstream iss(nnf);
      return parseDDNNF(iss, inputOrder);
    } catch(const CircuitException &) {
      throw;
    } catch(const std::exception &e) {
      throw CircuitException(std::string("KCMCP compile via '")+compiler
                             +"' failed: "+e.what());
    }
  }

  // A compiler that advertises the BC-S1.2 circuit input (KCMCP
  // "circuit-bcs12") is driven with native circuit input: the Boolean circuit
  // is sent directly instead of a Tseytin CNF, skipping the CNF transform and
  // the aux-variable reconciliation in the parse-back.  Requires a parser
  // that honours `I` declarations; on a gate shape BC-S1.2 cannot express, we
  // fall back to the Tseytin CNF path below.  (Today only d4v2 advertises it.)
  bool circuit_input = rec->acceptsInput("circuit-bcs12")
                       && !rec->argtpl_circuit.empty();
  if(find_external_tool(compiler_binary).empty())
    throw CircuitException(
            compiler_binary + " not found on PATH; install it or add its "
            "directory to provsql.tool_search_path");

  ScopedTempDir tmp;
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.nnf");
  // In circuit mode, inputOrder[v-1] is the IN gate for d4 variable v
  // (1-based); empty in CNF mode.
  std::vector<gate_t> inputOrder;
  std::string content;
  if(circuit_input) {
    try {
      content = BCS12(g, inputOrder);
    } catch(const CircuitException &) {
      // A gate shape BC-S1.2 cannot express: fall back to the CNF path.
      circuit_input = false;
      inputOrder.clear();
    }
  }
  if(!circuit_input)
    content = TseytinCNF(g, false);
  {
    std::ofstream ofs(filename);
    ofs << content;
  }

  if(provsql_verbose>=20) {
    provsql_notice("Tseytin circuit in %s", filename.c_str());
  }

  // The command line is the registry argtpl with {in}/{out} (and {binary})
  // substituted -- so a newly-registered compiler runs with its own
  // invocation, no per-name branch here.  When the BC-S1.2 circuit input is
  // in use, the record's argtpl_circuit is used instead (it pairs with the
  // circuit input written above and the circuit-mode variable resolution in
  // the parse-back).
  std::string cmdline;
  if(circuit_input)
    cmdline = provsql::expandCommandTemplate(rec->argtpl_circuit,
                                             compiler_binary, filename, outfilename);
  else
    cmdline = rec->buildCommand(filename, outfilename, compiler_binary);

  int retvalue=run_external_tool(cmdline);

  // run_external_tool runs the compiler in its own process group and
  // raises any pending cancel/terminate itself (after killing the child),
  // so a statement_timeout / pg_cancel_backend surfaces as 57014 here
  // rather than being masked by the "killed by signal" throw.
  //
  // (The pre-`-dDNNF` d4 CLI is no longer retried as a compiled-in special
  // case: a deployment still on it can register a tool with the old argtpl,
  // e.g. register_tool('d4-old', argtpl => '{in} -out={out}', ...).)
  CHECK_FOR_INTERRUPTS();

  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, compiler));

  // Read the result back with the parser the record advertises.  Panini's DD
  // format has its own reader; everything else is the tolerant NNF parser.
  if(rec->parser == "panini-dd")
    return parsePaniniDD(outfilename);

  std::ifstream ifs(outfilename.c_str());
  dDNNF dnnf = parseDDNNF(ifs, inputOrder);
  ifs.close();

  if(provsql_verbose>=20) {
    tmp.keep();
    provsql_notice("Compiled d-DNNF in %s", outfilename.c_str());
  }

  return dnnf;
}

// Parse a c2d/d4 NNF stream into a dDNNF over this circuit's input gates.
// Shared by the CLI compilation() path and the KCMCP client; see the header.
dDNNF BooleanCircuit::parseDDNNF(std::istream &in,
                                 const std::vector<gate_t> &inputOrder) const {
  const bool circuit_input = !inputOrder.empty();

  std::string line;
  getline(in,line);

  // Tolerant NNF detection (the single `nnf` parser): the classic c2d/d4
  // form opens with an "nnf <nodes> <edges> <vars>" magic line and roots at
  // the last node; the d4-family form has no magic line and roots at gate
  // "1".  A classic compiler emits the header iff satisfiable, so a missing
  // header on an empty file is an unsatisfiable formula; a missing header on
  // a non-empty file is the d4-family form.  (The d4v2 native-circuit path
  // also produces the header-less d4 form.)
  bool new_d4;
  if(line.rfind("nnf", 0) != 0) {
    if(line.empty()) {
      // unsatisfiable formula (empty output)
      return dDNNF();
    }
    new_d4 = true;
  } else {
    new_d4 = false;
    std::string nnf;
    unsigned nb_nodes, nb_edges, nb_variables;

    std::stringstream ss(line);
    ss >> nnf >> nb_nodes >> nb_edges >> nb_variables;

    if(nb_variables!=gates.size())
      throw CircuitException("Unreadable d-DNNF (wrong number of variables: " + std::to_string(nb_variables) +" vs " + std::to_string(gates.size()) + ")");

    getline(in,line);
  }

  dDNNF dnnf;

  // Map a d-DNNF literal's variable to the IN gate it stands for, if any.
  // CNF mode: variable = gate id + 1, real only for IN gates (every other
  // variable is a Tseytin auxiliary to skip). Circuit mode: variables 1..k
  // are the inputs in BCS12's declaration order, everything above k is an
  // internal-gate variable to skip.
  size_t k = inputOrder.size();
  auto resolveVar = [&](int v) -> std::pair<bool, gate_t> {
    unsigned idx = static_cast<unsigned>(abs(v));
    if(circuit_input) {
      if(idx>=1 && idx<=k)
        return {true, inputOrder[idx-1]};
      return {false, gate_t{}};
    }
    if(idx>=1 && (idx-1) < gates.size() && gates[idx-1]==BooleanGate::IN)
      return {true, static_cast<gate_t>(idx-1)};
    return {false, gate_t{}};
  };

  unsigned i=0;
  do {
    std::stringstream ss(line);

    std::string c;
    ss >> c;

    if(c=="O") {
      int var, args;
      ss >> var >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::OR);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="A") {
      int args;
      ss >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::AND);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="L") {
      int leaf;
      ss >> leaf;
      auto and_gate=dnnf.setGate(std::to_string(i), BooleanGate::AND);
      auto [is_in, in_gate] = resolveVar(leaf);
      if(is_in) {
        auto pid = static_cast<std::underlying_type<gate_t>::type>(in_gate);
        auto leaf_gate = dnnf.setGate(getUUID(in_gate), BooleanGate::IN, prob[pid]);
        if(leaf<0) {
          auto not_gate = dnnf.setGate(BooleanGate::NOT);
          dnnf.addWire(not_gate, leaf_gate);
          dnnf.addWire(and_gate, not_gate);
        } else {
          dnnf.addWire(and_gate, leaf_gate);
        }
      } else {
        ; // Do nothing, TRUE gate
      }
    } else if(c=="f" || c=="o") {
      // d4 extended format
      // A FALSE gate is an OR gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::OR);
    } else if(c=="t" || c=="a") {
      // d4 extended format
      // A TRUE gate is an AND gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::AND);
    } else if(dnnf.hasGate(c)) {
      // d4 extended format
      int var;
      ss >> var;
      auto id2=dnnf.getGate(std::to_string(var));

      std::vector<int> decisions;
      int decision;
      while(ss >> decision) {
        if(decision==0)
          break;
        // Edges carry decision literals over both real inputs and internal
        // variables (Tseytin auxiliaries in CNF mode, gate variables in
        // circuit mode). Keep only the input literals; the rest are
        // functionally determined and projected out (sound for probability).
        if(resolveVar(decision).first)
          decisions.push_back(decision);
      }

      if(decisions.empty()) {
        dnnf.addWire(dnnf.getGate(c), id2);
      } else {
        auto and_gate = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(dnnf.getGate(c), and_gate);
        dnnf.addWire(and_gate, id2);
        for(auto leaf : decisions) {
          auto in_gate = resolveVar(leaf).second;
          auto pid = static_cast<std::underlying_type<gate_t>::type>(in_gate);
          auto leaf_gate = dnnf.setGate(getUUID(in_gate), BooleanGate::IN, prob[pid]);
          if(leaf<0) {
            auto not_gate = dnnf.setGate(BooleanGate::NOT);
            dnnf.addWire(not_gate, leaf_gate);
            dnnf.addWire(and_gate, not_gate);
          } else {
            dnnf.addWire(and_gate, leaf_gate);
          }
        }
      }
    } else
      throw CircuitException(std::string("Unreadable d-DNNF (unknown node type: ")+c+")");

    ++i;
  } while(getline(in, line));

  dnnf.setRoot(dnnf.getGate(new_d4?"1":std::to_string(i-1)));

  // External NNF writers (c2d, minic2d, dsharp) leave TRUE constants
  // (empty AND gates) and FALSE constants (empty OR gates) embedded in
  // the structure, because their target formats (Decision-DNNF, SDD)
  // require every variable to be "covered" even when its value is
  // forced by the CNF. Run the standard peephole so the d-DNNF returned
  // to callers is in canonical form, matching the tree-decomposition
  // builder which already simplifies.
  dnnf.simplify();

  return dnnf;
}

// Generic weighted-model-counting runner.  Selects the counter from the
// registry by logical name, checks its binary and dependencies resolve,
// writes the weighted CNF in the convention its `parser` implies, runs the
// record's argtpl and reads the count back the same way -- so the four
// historical per-counter methods (ganak, sharpsat-td, dpmc, weightmc) become
// one, and a new wmc tool speaking a known convention is registrable without
// code.  The two conventions, keyed by `parser`:
//   wmc-line  MCC-2024 weighted DIMACS in ("c t wmc" + "c p weight" lines);
//             the count on a "c s exact" / "s wmc" line out.
//   weightmc  weightmc's own weighted DIMACS in; a "mantissa x 2^exp" out.
double BooleanCircuit::wmcCount(gate_t g, const std::string &requested,
                                const std::string &opt) const {
  // An empty tool name means "pick the best available counter" (highest
  // preference whose binary + dependencies resolve on PATH).
  std::string tool = requested;
  if(tool.empty()) {
    tool = selectTool("wmc");
    if(tool.empty())
      throw CircuitException(
              "no weighted model counter is available; install one (ganak, "
              "sharpsat-td, dpmc, weightmc) or add its directory to "
              "provsql.tool_search_path");
  }

  const provsql::ToolRecord *rec = provsql::tool_registry().find(tool);
  if(rec == nullptr || !rec->hasOperation("wmc"))
    throw CircuitException("Unknown wmc tool '" + tool + "'");
  if(!rec->enabled)
    throw CircuitException("Tool '" + tool + "' is disabled in the tool registry");

  // The binary (when the tool has one of its own) and every dependency must
  // resolve on PATH.  dpmc has no binary of its own -- it is the htb | dmc
  // pipeline named entirely in its template -- so its components are its
  // dependencies.
  if(!rec->binary.empty() && find_external_tool(rec->binary).empty())
    throw CircuitException(
            rec->binary + " not found on PATH; install it or add its "
            "directory to provsql.tool_search_path");
  for(const std::string &dep : rec->dependencies)
    if(find_external_tool(dep).empty())
      throw CircuitException(
              tool + " needs '" + dep + "' on PATH; install it or add its "
              "directory to provsql.tool_search_path");

  const bool weightmc_io = (rec->parser == "weightmc");

  ScopedTempDir tmp;
  const std::string &dirname = tmp.path();
  std::string filename    = tmp.file("input");
  std::string outfilename = tmp.file("input.out");
  {
    std::ofstream ofs(filename);
    if(weightmc_io) {
      // weightmc reads weights inline, in its own weighted-DIMACS dialect.
      ofs << TseytinCNF(g, true);
    } else {
      // MCC 2024 weighted DIMACS: a plain CNF plus per-input weight lines.
      ofs << "c t wmc\n";
      ofs << TseytinCNF(g, false);
      for(gate_t in : inputs) {
        int id = static_cast<int>(in) + 1;
        ofs << "c p weight " << id << ' ' << getProb(in)        << " 0\n";
        ofs << "c p weight -" << id << ' ' << (1.0 - getProb(in)) << " 0\n";
      }
    }
  }

  // {tmpdir} (sharpsat-td's flowcutter scratch) and {pivotAC} (weightmc's
  // approximation tolerance, from opt='delta;epsilon') are offered as
  // template placeholders; a tool that does not reference one ignores it.
  double epsilon = 0.8;
  {
    std::stringstream ssopt(opt);
    std::string delta_s, epsilon_s;
    getline(ssopt, delta_s, ';');
    getline(ssopt, epsilon_s, ';');
    try { double e = stod(epsilon_s); if(e != 0) epsilon = e; }
    catch(const std::exception &) {}
  }
  const double pivotAC = 2*ceil(exp(3./2)*(1+1/epsilon)*(1+1/epsilon));

  std::string cmdline = rec->buildCommand(
      filename, outfilename, rec->binary,
      {{"tmpdir", dirname}, {"pivotAC", std::to_string(pivotAC)}});

  int retvalue = run_external_tool(cmdline);
  CHECK_FOR_INTERRUPTS();
  if(retvalue)
    throw CircuitException(format_external_tool_status(retvalue, tool));

  std::ifstream ifs(outfilename.c_str());
  double ret;
  if(weightmc_io) {
    // weightmc prints the count as "<mantissa> x 2^<exp>" on its last line.
    std::string line, prev_line;
    while(getline(ifs, line)) prev_line = line;
    std::stringstream ss(prev_line);
    std::string result;
    ss >> result >> result >> result >> result >> result;
    std::istringstream iss(result);
    std::string val, exp;
    getline(iss, val, 'x');
    getline(iss, exp);
    if(exp.size() < 2)
      throw CircuitException("weightmc: could not parse '" + prev_line + "'");
    double value = stod(val);
    double exponent = stod(exp.substr(2));
    ret = value * pow(2.0, exponent);
  } else {
    // The count is on the last "c s exact ..." (or "s wmc ...") line;
    // parse_wmc_value tolerates the per-tool token layout on that line.
    std::string line, matched;
    while(getline(ifs, line))
      if(line.rfind("c s exact", 0) == 0 || line.rfind("s wmc", 0) == 0)
        matched = line;
    if(matched.empty())
      throw CircuitException(tool + ": could not find a count line in output");
    ret = parse_wmc_value(matched, tool.c_str());
  }

  if(provsql_verbose >= 20)
    tmp.keep();
  return ret;
}

#endif // external-tool compilation / counting (excluded from tdkc)

double BooleanCircuit::independentEvaluationInternal(
  gate_t g, std::set<gate_t> &seen,
  std::unordered_map<gate_t, double> &memo) const
{
  check_stack_depth(); // recurses on wires; guard deep circuits (see GenericCircuit::evaluate)
  // Memoised gates are variable-free (constant-only) -- returning the cached
  // value is sound (it touched nothing in `seen`) and avoids re-traversing a
  // shared constant subgraph.  A variable-bearing gate is never cached, so a
  // second visit re-enters its subtree and throws on the repeated variable.
  {
    auto it = memo.find(g);
    if(it != memo.end())
      return it->second;
  }

  // A certified gate (see DNNF_CERT_INFO) opens a maximal island, walked
  // iteratively (certified circuits can be as deep as the data).  A
  // certified gate reached a second time -- from another island or from
  // the uncertified region -- is re-walked: its variables then hit `seen`
  // and the evaluation throws, so entanglement across islands is
  // conservatively rejected, exactly like a read-once violation.
  if(isDNNFCertified(g))
    return evaluateCertifiedIsland(g, seen, memo);

  const std::size_t seen_before = seen.size();

  double result=1.;

  switch(getGateType(g)) {
  case BooleanGate::AND:
    for(const auto &c: getWires(g)) {
      result*=independentEvaluationInternal(c, seen, memo);
    }
    break;

  case BooleanGate::OR:
  {
    // We collect probability among each group of children, where we
    // group MULIN gates with the same key var together
    std::map<gate_t, double> groups;
    std::set<gate_t> local_mulins;
    std::set<std::pair<gate_t, unsigned> > mulin_seen;

    for(const auto &c: getWires(g)) {
      auto group = c;
      if(getGateType(c) == BooleanGate::MULIN) {
        group = *getWires(c).begin();
        if(local_mulins.find(group)==local_mulins.end()) {
          if(seen.find(group)!=seen.end())
            throw CircuitException("Not an independent circuit");
          else
            seen.insert(group);
          local_mulins.insert(group);
        }
        auto p = std::make_pair(group, getInfo(c));
        if(mulin_seen.find(p)==mulin_seen.end()) {
          groups[group] += getProb(c);
          mulin_seen.insert(p);
        }
      } else
        groups[group] = independentEvaluationInternal(c, seen, memo);
    }

    for(const auto [k, v]: groups)
      result *= 1-v;
    result = 1-result;
  }
  break;

  case BooleanGate::NOT:
    result=1-independentEvaluationInternal(*getWires(g).begin(), seen, memo);
    break;

  case BooleanGate::IN:
  {
    /* A leaf with probability 0 or 1 is a constant : it carries no
     * Boolean variable that can collide with another occurrence of
     * itself.  Skip the seen-set bookkeeping so circuits where the
     * shared subgraphs are all constants (e.g. RangeCheck-resolved
     * comparators flowing through a non-tree structure, or
     * user-flipped Bernoullis pinned to 0 / 1) stay evaluable under
     * the read-once `independent` method.  Anything strictly between
     * 0 and 1 is a real Bernoulli variable and must remain
     * read-once. */
    const double p = getProb(g);
    if (p == 0.0 || p == 1.0) {
      result = p;
      break;
    }
    if(seen.find(g)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(g);
    result=p;
  }
  break;

  case BooleanGate::MULIN:
  {
    auto child = *getWires(g).begin();
    if(seen.find(child)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(child);
    result=getProb(g);
  }
  break;

  case BooleanGate::UNDETERMINED:
  case BooleanGate::MULVAR:
    throw CircuitException("Bad gate");
  }

  // Cache only if this gate consumed no variable (constant-only subgraph).
  if(seen.size() == seen_before)
    memo[g] = result;
  return result;
}

double BooleanCircuit::evaluateCertifiedIsland(
  gate_t root, std::set<gate_t> &seen,
  std::unordered_map<gate_t, double> &memo) const
{
  const std::size_t seen_before = seen.size();
  // Island-local values: within the island every gate is computed once
  // (sharing is licensed by the certificate), and the explicit post-order
  // stack keeps the walk safe on circuits as deep as the data.
  std::unordered_map<gate_t, double> val;
  std::vector<gate_t> stack{root};

  while(!stack.empty()) {
    const gate_t g = stack.back();
    if(val.find(g) != val.end()) {
      stack.pop_back();
      continue;
    }

    const auto t = getGateType(g);

    if(t == BooleanGate::IN) {
      // Same constant-leaf exemption as the read-once walk: a 0/1 leaf
      // carries no Boolean variable.
      const double p = getProb(g);
      if(p != 0.0 && p != 1.0) {
        if(seen.find(g) != seen.end())
          throw CircuitException("Not an independent circuit");
        seen.insert(g);
      }
      val[g] = p;
      stack.pop_back();
      continue;
    }
    if(t == BooleanGate::MULIN) {
      auto child = *getWires(g).begin();
      if(seen.find(child) != seen.end())
        throw CircuitException("Not an independent circuit");
      seen.insert(child);
      val[g] = getProb(g);
      stack.pop_back();
      continue;
    }
    if(t != BooleanGate::NOT && !isDNNFCertified(g)) {
      // Uncertified gate inside the island: standard read-once rules (its
      // own certified descendants open fresh sub-islands).
      val[g] = independentEvaluationInternal(g, seen, memo);
      stack.pop_back();
      continue;
    }

    bool ready = true;
    for(const auto &c: getWires(g))
      if(val.find(c) == val.end()) {
        stack.push_back(c);
        ready = false;
      }
    if(!ready)
      continue;

    double result;
    if(t == BooleanGate::NOT)
      result = 1 - val[getWires(g)[0]];
    else if(t == BooleanGate::AND) {
      // Certified decomposable AND: product.
      result = 1.;
      for(const auto &c: getWires(g))
        result *= val[c];
    } else {
      // Certified deterministic OR: plain sum (mutual exclusivity).
      result = 0.;
      for(const auto &c: getWires(g))
        result += val[c];
    }
    val[g] = result;
    stack.pop_back();
  }

  // Same global-memo rule as the recursive walk: cache only when the
  // island consumed no variable.
  if(seen.size() == seen_before)
    memo[root] = val[root];
  return val[root];
}

double BooleanCircuit::independentEvaluation(gate_t g) const
{
  std::set<gate_t> seen;
  std::unordered_map<gate_t, double> memo;
  return independentEvaluationInternal(g, seen, memo);
}

void BooleanCircuit::setInfo(gate_t g, unsigned int i)
{
  info[g] = i;
}

unsigned BooleanCircuit::getInfo(gate_t g) const
{
  auto it = info.find(g);

  if(it==info.end())
    return 0;
  else
    return it->second;
}

void BooleanCircuit::rewriteMultivaluedGatesRec(
  const std::vector<gate_t> &muls,
  const std::vector<double> &cumulated_probs,
  unsigned start,
  unsigned end,
  std::vector<gate_t> &prefix)
{
  if(start==end) {
    getWires(muls[start]) = prefix;
    return;
  }

  unsigned mid = (start+end)/2;
  // cumulated_probs is an *inclusive* prefix sum (cumulated_probs[i] =
  // p[0]+...+p[i]).  The conditional probability of being in the left
  // half [start..mid] given the range [start..end] is therefore
  //   (cum[mid] - cum[start-1]) / (cum[end] - cum[start-1])
  // with cum[-1] treated as 0 when start==0.
  double prev_start = (start == 0) ? 0. : cumulated_probs[start - 1];
  auto g = setGate(
    BooleanGate::IN,
    (cumulated_probs[mid] - prev_start) /
    (cumulated_probs[end] - prev_start));
  auto not_g = setGate(BooleanGate::NOT);
  getWires(not_g).push_back(g);

  prefix.push_back(g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, start, mid, prefix);
  prefix.pop_back();
  prefix.push_back(not_g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, mid+1, end, prefix);
  prefix.pop_back();
}

/**
 * @brief Check whether two double values are approximately equal.
 * @param a  First value.
 * @param b  Second value.
 * @return   @c true if @p a and @p b differ by less than 10× machine epsilon.
 */
static constexpr bool almost_equals(double a, double b)
{
  double diff = a - b;
  constexpr double epsilon = std::numeric_limits<double>::epsilon() * 10;

  return (diff < epsilon && diff > -epsilon);
}

void BooleanCircuit::rewriteMultivaluedGates()
{
  std::map<gate_t,std::vector<gate_t> > var2mulinput;
  for(auto mul: mulinputs) {
    var2mulinput[*getWires(mul).begin()].push_back(mul);
  }
  mulinputs.clear();

  for(const auto &[var, muls]: var2mulinput)
  {
    const unsigned n = muls.size();
    std::vector<double> cumulated_probs(n);
    double cumulated_prob=0.;

    for(unsigned i=0; i<n; ++i) {
      cumulated_prob += getProb(muls[i]);
      cumulated_probs[i] = cumulated_prob;
      gates[static_cast<std::underlying_type<gate_t>::type>(muls[i])] = BooleanGate::AND;
      getWires(muls[i]).clear();
    }

    std::vector<gate_t> prefix;
    prefix.reserve(static_cast<unsigned>(log(n)/log(2)+2));
    if(!almost_equals(cumulated_probs[n-1],1.)) {
      prefix.push_back(setGate(BooleanGate::IN, cumulated_probs[n-1]));
    }
    rewriteMultivaluedGatesRec(muls, cumulated_probs, 0, n-1, prefix);
  }
}

gate_t BooleanCircuit::interpretAsDDInternal(gate_t g, std::set<gate_t> &seen, dDNNF &dd) const {
  check_stack_depth(); // recurses on wires; guard deep circuits (see GenericCircuit::evaluate)

  // A certified gate (see DNNF_CERT_INFO) opens a maximal island, copied
  // iteratively with native deterministic ORs; the recursion only walks
  // the uncertified region.
  if(isDNNFCertified(g))
    return interpretCertifiedIsland(g, seen, dd);

  gate_t dg{0};

  switch(getGateType(g)) {
  case BooleanGate::AND:
  {
    dg = dd.setGate(BooleanGate::AND);
    for(const auto &c: getWires(g)) {
      auto dc = interpretAsDDInternal(c, seen, dd);
      dd.addWire(dg, dc);
    }
  }
  break;

  case BooleanGate::OR:
  {
    dg = dd.setGate(BooleanGate::NOT);
    auto dng = dd.setGate(BooleanGate::AND);
    dd.addWire(dg, dng);
    for(const auto &c: getWires(g)) {
      auto dc = interpretAsDDInternal(c, seen, dd);
      auto dnc = dd.setGate(BooleanGate::NOT);
      dd.addWire(dnc, dc);
      dd.addWire(dng, dnc);
    }
  }
  break;

  case BooleanGate::NOT:
  {
    dg = dd.setGate(BooleanGate::NOT);
    auto dc = interpretAsDDInternal(getWires(g)[0], seen, dd);
    dd.addWire(dg, dc);
  }
  break;

  case BooleanGate::IN:
    if(seen.find(g)!=seen.end())
      throw CircuitException("Not an independent circuit");
    seen.insert(g);
    if(getUUID(g).empty())
      dg = dd.setGate(BooleanGate::IN, getProb(g));
    else
      dg = dd.setGate(getUUID(g), BooleanGate::IN, getProb(g));
    break;

  case BooleanGate::MULIN:
  case BooleanGate::MULVAR:
  case BooleanGate::UNDETERMINED:
    throw CircuitException("Unsupported gate in interpretAsDD");
  }

  return dg;
}

gate_t BooleanCircuit::interpretCertifiedIsland(gate_t root,
                                                std::set<gate_t> &seen,
                                                dDNNF &dd) const
{
  // Same iterative island walk as evaluateCertifiedIsland, building dd
  // gates instead of probabilities; shared sub-circuits map to shared dd
  // gates, and the copied gates keep their certificate so the produced
  // artefact remains self-describing.
  std::unordered_map<gate_t, gate_t> val;
  std::vector<gate_t> stack{root};

  while(!stack.empty()) {
    const gate_t g = stack.back();
    if(val.find(g) != val.end()) {
      stack.pop_back();
      continue;
    }

    const auto t = getGateType(g);

    if(t == BooleanGate::IN) {
      if(seen.find(g) != seen.end())
        throw CircuitException("Not an independent circuit");
      seen.insert(g);
      val[g] = getUUID(g).empty()
               ? dd.setGate(BooleanGate::IN, getProb(g))
               : dd.setGate(getUUID(g), BooleanGate::IN, getProb(g));
      stack.pop_back();
      continue;
    }
    if(t != BooleanGate::NOT && !isDNNFCertified(g)) {
      // Uncertified gate inside the island: standard rules (its own
      // certified descendants open fresh sub-islands).
      val[g] = interpretAsDDInternal(g, seen, dd);
      stack.pop_back();
      continue;
    }

    bool ready = true;
    for(const auto &c: getWires(g))
      if(val.find(c) == val.end()) {
        stack.push_back(c);
        ready = false;
      }
    if(!ready)
      continue;

    gate_t dg;
    if(t == BooleanGate::NOT) {
      dg = dd.setGate(BooleanGate::NOT);
      dd.addWire(dg, val[getWires(g)[0]]);
    } else {
      dg = dd.setGate(t);
      dd.setInfo(dg, DNNF_CERT_INFO);
      for(const auto &c: getWires(g))
        dd.addWire(dg, val[c]);
    }
    val[g] = dg;
    stack.pop_back();
  }

  return val[root];
}

dDNNF BooleanCircuit::interpretAsDD(gate_t g) const
{
  dDNNF dd;
  std::set<gate_t> seen;

  dd.setRoot(interpretAsDDInternal(g, seen, dd));

  // The OR-as-NOT(AND(NOT, ...)) De Morgan rewriting above introduces
  // many redundant NOT-NOT pairs and single-child AND/OR gates that the
  // canonical simplify pass folds away; matches what the external-KC
  // and tree-decomposition paths already do.
  dd.simplify();

  return dd;
}

// makeDD / makeDDByName fall back to the external compilers, so they too are
// excluded from the external-tool-free tdkc build.
#ifndef TDKC
dDNNF BooleanCircuit::makeDD(gate_t g, const std::string &method, const std::string &args) const
{
  if(method=="compilation") {
    return compilation(g, args);
  } else if(method=="tree-decomposition") {
    try {
      TreeDecomposition td(*this);
      return dDNNFTreeDecompositionBuilder{
        *this, g, td}.build();
    } catch(TreeDecompositionException &) {
      provsql_error("Treewidth greater than %u", TreeDecomposition::MAX_TREEWIDTH);
    }
  } else if(method=="interpret-as-dd") {
    return interpretAsDD(g);
  } else {
    dDNNF dd;
    try {
      dd = interpretAsDD(g);
      if(provsql_verbose>=20)
        provsql_notice("Circuit interpreted as dD, %ld gates", dd.getNbGates());
    } catch(CircuitException &) {
      try {
        TreeDecomposition td(*this);
        dd = dDNNFTreeDecompositionBuilder{
          *this, g, td}.build();
        if(provsql_verbose>=25)
          provsql_notice("dD obtained by tree decomposition, %ld gates", dd.getNbGates());
      } catch(TreeDecompositionException &) {
        // Last-resort fallback: chooseCompiler() prefers
        // provsql.fallback_compiler when available, else the highest-
        // preference compiler on PATH.
        std::string chosen = chooseCompiler();
        dd = compilation(g, chosen);
        if(provsql_verbose>=20)
          provsql_notice("dD obtained by compilation using %s, %ld gates",
                         chosen.c_str(), dd.getNbGates());
      }
    }

    return dd;
  }
}

dDNNF BooleanCircuit::makeDDByName(gate_t g, const std::string &name) const
{
  // In-process meta-routes are dispatched through makeDD: "default" runs the
  // whole fallback chain, "tree-decomposition" / "interpret-as-dd" the single
  // route.  Everything else goes to compilation(); the empty string there
  // means "pick the highest-preference available compiler" (so a no-compiler
  // request uses the registry instead of a hardcoded d4).
  if(name=="default" || name=="tree-decomposition" || name=="interpret-as-dd")
    return makeDD(g, name=="default" ? std::string() : name, "");
  return compilation(g, name);
}
#endif // makeDD / makeDDByName (excluded from tdkc)
