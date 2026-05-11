/**
 * @file HybridEvaluator.cpp
 * @brief Implementation of the peephole simplifier.
 *        See @c HybridEvaluator.h for the full docstring.
 */
#include "HybridEvaluator.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "MonteCarloSampler.h"  // monteCarloRV
#include "RandomVariable.h"     // parse_distribution_spec, parseDoubleStrict, DistKind
extern "C" {
#include "provsql_utils.h"      // gate_type, provsql_arith_op
}

namespace provsql {

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/**
 * @brief Format a double back into the canonical text form used by
 *        @c gate_value extras.
 *
 * Default-float format at precision 17 round-trips through
 * @c std::stod: doubles need at most 17 significant decimal digits
 * to be exactly recoverable.  Round-numbered values (e.g. 0.5, 2.0)
 * print with the minimal representation under @c defaultfloat, so
 * the test output stays clean for the common analytical cases.
 *
 * @c std::ostringstream is used rather than @c std::snprintf because
 * including @c <cstdio> after PostgreSQL's @c port.h would expand
 * @c std::snprintf to the non-existent @c std::pg_snprintf via the
 * @c #define snprintf macro in @c port.h.
 */
std::string double_to_text(double v)
{
  std::ostringstream oss;
  oss << std::setprecision(17) << v;
  return oss.str();
}

/**
 * @brief Try to evaluate a @c gate_arith subtree to a scalar constant.
 *
 * Recurses over the @c gate_arith ops, parsing @c gate_value leaves
 * via @c parseDoubleStrict.  Returns @c NaN if any leaf is not a
 * @c gate_value (or fails to parse), if a binary op has the wrong
 * arity, or if any arith op is unknown.  Successful constants of any
 * value (including @c 0 and @c NaN-shaped values via division) are
 * returned as @c double literals; the caller distinguishes
 * "couldn't fold" from "folded to NaN" via @c std::isnan on the
 * input gate's children, not on the result.  In practice provsql
 * @c gate_value extras never carry @c NaN, so the @c NaN-as-sentinel
 * convention is unambiguous.
 */
double try_eval_constant(const GenericCircuit &gc, gate_t g)
{
  auto t = gc.getGateType(g);
  if (t == gate_value) {
    try { return parseDoubleStrict(gc.getExtra(g)); }
    catch (const CircuitException &) { return NaN; }
  }
  if (t != gate_arith) return NaN;

  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  const auto &wires = gc.getWires(g);
  if (wires.empty()) return NaN;

  double first = try_eval_constant(gc, wires[0]);
  if (std::isnan(first)) return NaN;

  switch (op) {
    case PROVSQL_ARITH_PLUS: {
      double r = first;
      for (std::size_t i = 1; i < wires.size(); ++i) {
        double v = try_eval_constant(gc, wires[i]);
        if (std::isnan(v)) return NaN;
        r += v;
      }
      return r;
    }
    case PROVSQL_ARITH_TIMES: {
      double r = first;
      for (std::size_t i = 1; i < wires.size(); ++i) {
        double v = try_eval_constant(gc, wires[i]);
        if (std::isnan(v)) return NaN;
        r *= v;
      }
      return r;
    }
    case PROVSQL_ARITH_MINUS: {
      if (wires.size() != 2) return NaN;
      double v = try_eval_constant(gc, wires[1]);
      if (std::isnan(v)) return NaN;
      return first - v;
    }
    case PROVSQL_ARITH_DIV: {
      if (wires.size() != 2) return NaN;
      double v = try_eval_constant(gc, wires[1]);
      if (std::isnan(v)) return NaN;
      return first / v;
    }
    case PROVSQL_ARITH_NEG:
      if (wires.size() != 1) return NaN;
      return -first;
  }
  return NaN;
}

/**
 * @brief Rewrite @p g in place as a @c gate_value carrying @p c.
 *
 * Clears wires and infos; the old children become orphans (no parent
 * reaches them via @p g anymore).  This is the same pattern
 * @c resolveCmpToBernoulli uses for resolved comparators.
 */
void replace_with_value(GenericCircuit &gc, gate_t g, double c)
{
  gc.resolveToValue(g, double_to_text(c));
}

/**
 * @brief Rewrite @p g in place as a normal @c gate_rv with parameters
 *        @p mean and @p sigma.
 *
 * Used by the normal-family closure when a PLUS over linear
 * combinations of independent normals folds to a single normal.
 * Sigma is the standard deviation (consistent with the on-disk
 * @c "normal:μ,σ" encoding).
 */
void replace_with_normal_rv(GenericCircuit &gc, gate_t g,
                            double mean, double sigma)
{
  gc.resolveToRv(g, "normal:" + double_to_text(mean)
                    + "," + double_to_text(sigma));
}

/**
 * @brief Rewrite @p g in place as an Erlang @c gate_rv with shape
 *        @p k and rate @p lambda.
 */
void replace_with_erlang_rv(GenericCircuit &gc, gate_t g,
                            unsigned long k, double lambda)
{
  gc.resolveToRv(g, "erlang:" + std::to_string(k)
                    + "," + double_to_text(lambda));
}

/**
 * @brief Test whether wire @p g is a @c gate_value parseable to
 *        scalar @p target (within bit-exact equality).
 */
bool is_value_equal_to(const GenericCircuit &gc, gate_t g, double target)
{
  if (gc.getGateType(g) != gate_value) return false;
  try { return parseDoubleStrict(gc.getExtra(g)) == target; }
  catch (const CircuitException &) { return false; }
}

/**
 * @brief Identity-element drop for @c PLUS / @c TIMES.
 *
 * - @c PLUS: drop @c gate_value:0 wires.  If 0 wires remain, fold to
 *   @c gate_value:0.
 * - @c TIMES: if any wire is @c gate_value:0, fold to @c gate_value:0
 *   (multiplicative absorber, even if other wires are non-constant).
 *   Otherwise drop @c gate_value:1 wires; if 0 wires remain, fold to
 *   @c gate_value:1.
 *
 * Returns @c true if @p g was mutated.  After a mutation that leaves
 * @p g as @c gate_arith, the per-gate fixed-point loop in @c simplify
 * re-runs the rules: a @c PLUS that had three wires reduced to one
 * looks the same as the original input to the simplifier, so we just
 * need to terminate when no rule fires.
 */
bool try_identity_drop(GenericCircuit &gc, gate_t g)
{
  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  auto &wires = gc.getWires(g);

  if (op == PROVSQL_ARITH_PLUS) {
    std::vector<gate_t> kept;
    kept.reserve(wires.size());
    for (gate_t w : wires) {
      if (!is_value_equal_to(gc, w, 0.0)) kept.push_back(w);
    }
    if (kept.size() == wires.size()) return false;  /* nothing to drop */
    if (kept.empty()) {
      replace_with_value(gc, g, 0.0);
      return true;
    }
    wires = std::move(kept);
    return true;
  }

  if (op == PROVSQL_ARITH_TIMES) {
    for (gate_t w : wires) {
      if (is_value_equal_to(gc, w, 0.0)) {
        replace_with_value(gc, g, 0.0);
        return true;
      }
    }
    std::vector<gate_t> kept;
    kept.reserve(wires.size());
    for (gate_t w : wires) {
      if (!is_value_equal_to(gc, w, 1.0)) kept.push_back(w);
    }
    if (kept.size() == wires.size()) return false;
    if (kept.empty()) {
      replace_with_value(gc, g, 1.0);
      return true;
    }
    wires = std::move(kept);
    return true;
  }

  return false;
}

/**
 * @brief Footprint of base @c gate_rv UUIDs reachable below @p g
 *        through @c gate_arith composition only.
 *
 * Stops at non-arith gates (including @c gate_value, which contribute
 * no RV identity).  Used by the normal-family closure as the
 * independence test between sibling PLUS-wires.
 */
void collect_rv_footprint(const GenericCircuit &gc, gate_t g,
                          std::unordered_set<gate_t> &fp,
                          std::unordered_set<gate_t> &seen)
{
  if (!seen.insert(g).second) return;
  auto t = gc.getGateType(g);
  if (t == gate_rv) {
    fp.insert(g);
    return;
  }
  if (t == gate_arith) {
    for (gate_t c : gc.getWires(g))
      collect_rv_footprint(gc, c, fp, seen);
  }
}

/**
 * @brief Decomposition of a PLUS-wire as @c a*Z + b for the
 *        normal-family closure.
 *
 * - @c rv_gate == invalid (sentinel @c (gate_t)-1) ⇒ pure constant
 *   wire: contributes @p b to the total mean, 0 to the total
 *   variance, and no RV to the footprint.
 * - @c rv_gate != invalid ⇒ scalar-multiple-of-normal wire:
 *   contributes @c a*μ + b to the total mean, @c a²σ² to the total
 *   variance, and @p rv_gate to the footprint.
 */
struct LinearTerm {
  gate_t rv_gate;     ///< Base normal gate_rv, or invalid for constants.
  double a;           ///< Scalar multiplier (0 for pure constants).
  double b;           ///< Additive offset (0 for pure RV wires).
};

constexpr gate_t INVALID_GATE = static_cast<gate_t>(-1);

bool is_invalid(gate_t g) { return g == INVALID_GATE; }

/**
 * @brief Try to interpret @p g as @c a*Z + b for a single normal RV.
 *
 * Recognised shapes:
 * - bare normal @c gate_rv:           @c (Z=g, a=1, b=0)
 * - bare @c gate_value:               @c (Z=invalid, a=0, b=value)
 * - @c arith(NEG, child):             negate the child's decomposition
 * - @c arith(TIMES, value:c, child):  scale the child's decomposition
 *   by @c c (and symmetrically @c arith(TIMES, child, value:c)).
 *   Only 2-wire @c TIMES with exactly one @c gate_value side is
 *   recognised; other shapes fall through to "not decomposable".
 *
 * Nested @c arith(PLUS, ...) children of the outer PLUS are not
 * decomposed by this routine: the bottom-up simplifier already
 * folded them before the outer PLUS is processed, so by the time
 * we examine the outer PLUS its children are either leaves or
 * non-foldable arith.  An undecomposable wire causes the whole
 * normal closure to bail.
 */
std::optional<LinearTerm>
decompose_normal_term(const GenericCircuit &gc, gate_t g)
{
  auto t = gc.getGateType(g);

  if (t == gate_value) {
    double v;
    try { v = parseDoubleStrict(gc.getExtra(g)); }
    catch (const CircuitException &) { return std::nullopt; }
    return LinearTerm{INVALID_GATE, 0.0, v};
  }

  if (t == gate_rv) {
    auto spec = parse_distribution_spec(gc.getExtra(g));
    if (!spec || spec->kind != DistKind::Normal) return std::nullopt;
    return LinearTerm{g, 1.0, 0.0};
  }

  if (t != gate_arith) return std::nullopt;

  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  const auto &wires = gc.getWires(g);

  /* After an identity-element drop, a PLUS or TIMES gate can be left
   * with a single wire that semantically passes through.  Recurse so
   * the outer closure can still see the underlying term.  We can't
   * fold the singleton wrapper away in place (rewriting it as the
   * child's type / extra would mint a fresh RV identity and break
   * per-iteration MC memoisation across other parents of the child),
   * but the outer closure rewrites the OUTER gate, which is safe. */
  if ((op == PROVSQL_ARITH_PLUS || op == PROVSQL_ARITH_TIMES)
      && wires.size() == 1) {
    return decompose_normal_term(gc, wires[0]);
  }

  if (op == PROVSQL_ARITH_NEG) {
    if (wires.size() != 1) return std::nullopt;
    auto inner = decompose_normal_term(gc, wires[0]);
    if (!inner) return std::nullopt;
    return LinearTerm{inner->rv_gate, -inner->a, -inner->b};
  }

  if (op == PROVSQL_ARITH_TIMES) {
    if (wires.size() != 2) return std::nullopt;
    /* Identify the constant side and the variable side. */
    double c = NaN;
    gate_t var_side = INVALID_GATE;
    if (gc.getGateType(wires[0]) == gate_value) {
      try { c = parseDoubleStrict(gc.getExtra(wires[0])); }
      catch (const CircuitException &) { return std::nullopt; }
      var_side = wires[1];
    } else if (gc.getGateType(wires[1]) == gate_value) {
      try { c = parseDoubleStrict(gc.getExtra(wires[1])); }
      catch (const CircuitException &) { return std::nullopt; }
      var_side = wires[0];
    } else {
      return std::nullopt;
    }
    auto inner = decompose_normal_term(gc, var_side);
    if (!inner) return std::nullopt;
    return LinearTerm{inner->rv_gate, c * inner->a, c * inner->b};
  }

  return std::nullopt;
}

/**
 * @brief Normal-family closure on a @c PLUS gate.
 *
 * If every wire decomposes to @c a*Z + b for an independent normal
 * @c Z, replaces the gate with a single normal @c gate_rv whose
 * parameters are the closed-form combinations.  Independence is
 * tested by collecting the base-RV footprint of each contributing
 * normal and requiring pairwise-disjoint footprints; the
 * @c decompose_normal_term restriction to bare normal leaves makes
 * the footprint just @c {Z_i} for each non-constant wire, so the
 * test reduces to "all @c Z_i are distinct UUIDs".
 *
 * When every wire is a pure constant (all RV-side empty), the closure
 * is just the constant fold and we let the dedicated path handle it
 * &mdash; this routine returns @c false so the fixed-point loop
 * re-runs and the constant fold fires next.
 */
bool try_normal_closure(GenericCircuit &gc, gate_t g)
{
  const auto &wires = gc.getWires(g);
  if (wires.size() < 2) return false;

  std::vector<LinearTerm> terms;
  terms.reserve(wires.size());
  for (gate_t w : wires) {
    auto term = decompose_normal_term(gc, w);
    if (!term) return false;
    terms.push_back(*term);
  }

  /* Independence test: every non-constant term must have a distinct
   * Z gate_t.  We also need at least one non-constant term (otherwise
   * this is the pure-constant case and constant folding handles it). */
  std::unordered_set<gate_t> seen_rvs;
  bool has_rv = false;
  for (const auto &t : terms) {
    if (is_invalid(t.rv_gate)) continue;
    has_rv = true;
    if (!seen_rvs.insert(t.rv_gate).second) return false;  /* dependent */
  }
  if (!has_rv) return false;

  double total_mean = 0.0;
  double total_var  = 0.0;
  for (const auto &t : terms) {
    total_mean += t.b;
    if (is_invalid(t.rv_gate)) continue;
    auto spec = parse_distribution_spec(gc.getExtra(t.rv_gate));
    if (!spec || spec->kind != DistKind::Normal) return false;
    const double mu    = spec->p1;
    const double sigma = spec->p2;
    total_mean += t.a * mu;
    total_var  += t.a * t.a * sigma * sigma;
  }

  /* Degenerate variance ⇒ the closure produces a Dirac at total_mean.
   * We can keep this as a normal with σ=0, but the existing constructor
   * silently routes σ=0 through @c as_random, and downstream consumers
   * may not all handle σ=0 gracefully.  Skip and let other passes deal
   * with it (in practice this branch is unreachable: we required at
   * least one a≠0 term, and σ=0 normals are constructed as gate_value
   * by @c provsql.normal, so total_var > 0 whenever the closure fires). */
  if (total_var <= 0.0) return false;

  replace_with_normal_rv(gc, g, total_mean, std::sqrt(total_var));
  return true;
}

/**
 * @brief Erlang-family closure on a @c PLUS gate.
 *
 * Fires only on the strict shape <tt>PLUS(E1, ..., Ek)</tt> with
 * k ≥ 2, each @c Ei a bare exponential @c gate_rv leaf, all rates
 * equal, all UUIDs distinct.  Replaces the gate with a single
 * Erlang(k, λ) @c gate_rv.  Mixed exponential/non-exponential wires
 * or different rates leave the gate untouched (hypoexponential is
 * outside the simplifier's family-closure scope; the sampler handles
 * those via per-iteration draws).
 */
bool try_erlang_closure(GenericCircuit &gc, gate_t g)
{
  const auto &wires = gc.getWires(g);
  if (wires.size() < 2) return false;

  /* Accept any mix of bare Exp(λ) and Erlang(k, λ) gate_rv leaves
   * with the same λ and pairwise-distinct UUIDs.  Left-associative
   * parsing of `a + b + c` builds `(a+b)+c` which bottom-up
   * simplifies to Erlang(2)+c, so the closure has to recognise the
   * Erlang+Exp shape to close the chain.  Erlang(k1) + Erlang(k2) =
   * Erlang(k1+k2) for the same rate; Exp is the k=1 case. */
  double lambda = NaN;
  unsigned long total_shape = 0;
  std::unordered_set<gate_t> seen;
  for (gate_t w : wires) {
    if (gc.getGateType(w) != gate_rv) return false;
    auto spec = parse_distribution_spec(gc.getExtra(w));
    if (!spec) return false;
    double w_lambda;
    unsigned long w_shape;
    if (spec->kind == DistKind::Exponential) {
      w_lambda = spec->p1;
      w_shape  = 1;
    } else if (spec->kind == DistKind::Erlang) {
      /* Integer k stored in p1; non-integer is rejected upstream by
       * the constructor, but guard defensively here so a corrupted
       * extra cannot trigger an invalid shape sum. */
      if (spec->p1 < 1.0 || spec->p1 != std::floor(spec->p1)) return false;
      w_lambda = spec->p2;
      w_shape  = static_cast<unsigned long>(spec->p1);
    } else {
      return false;
    }
    if (!seen.insert(w).second)   return false;      /* shared UUID */
    if (std::isnan(lambda))       lambda = w_lambda;
    else if (lambda != w_lambda)  return false;      /* different rate */
    total_shape += w_shape;
  }

  replace_with_erlang_rv(gc, g, total_shape, lambda);
  return true;
}

/**
 * @brief Run the per-gate fixed-point loop.
 *
 * After each rule succeeds the gate is re-evaluated under every rule,
 * so a single bottom-up pass collapses nested foldable structures
 * (e.g. <tt>arith(NEG, arith(PLUS, value, value))</tt>) in one go.
 *
 * @return Number of rewrites performed on this gate.
 */
unsigned apply_rules(GenericCircuit &gc, gate_t g)
{
  unsigned local = 0;
  /* Iteration bound: each rule strictly shrinks the gate (fewer wires
   * or simpler type), so the loop terminates in O(#initial wires)
   * iterations.  The bound is defensive insurance against an
   * unintended infinite loop. */
  for (unsigned iter = 0; iter < 32; ++iter) {
    if (gc.getGateType(g) != gate_arith) break;

    /* 1. Constant folding (collapses any all-gate_value arith). */
    {
      double c = try_eval_constant(gc, g);
      if (!std::isnan(c)) {
        replace_with_value(gc, g, c);
        ++local;
        break;
      }
    }

    /* 2. Identity / absorber drops on PLUS and TIMES. */
    if (try_identity_drop(gc, g)) {
      ++local;
      continue;
    }

    /* 3. Family closures on PLUS. */
    auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
    if (op == PROVSQL_ARITH_PLUS) {
      if (try_normal_closure(gc, g)) { ++local; break; }
      if (try_erlang_closure(gc, g)) { ++local; break; }
    }

    break;  /* no rule fired this iteration */
  }
  return local;
}

/**
 * @brief Post-order DFS that simplifies every reachable gate.
 *
 * Children are simplified before parents so by the time a gate is
 * examined its wires already reflect any rewrites: the bottom-up
 * order is essential for cascading folds (a parent PLUS over a child
 * arith that just folded to a gate_value gets a chance to fold that
 * constant away).
 */
void simplify(GenericCircuit &gc, gate_t g,
              std::unordered_set<gate_t> &done, unsigned &counter)
{
  /* Iterative DFS with an explicit stack: the natural recursive form
   * blew the host stack on deeply-nested arith chains in early
   * experiments; iteration with a small per-node bookkeeping triple
   * (gate, child-cursor, processed-flag) keeps the cost in heap. */
  std::stack<std::pair<gate_t, std::size_t>> stk;
  if (!done.insert(g).second) return;
  stk.emplace(g, 0);

  while (!stk.empty()) {
    auto &frame = stk.top();
    gate_t cur = frame.first;
    const auto &wires = gc.getWires(cur);
    if (frame.second < wires.size()) {
      gate_t child = wires[frame.second++];
      if (done.insert(child).second) stk.emplace(child, 0);
      continue;
    }
    /* All children processed; apply rules to cur. */
    if (gc.getGateType(cur) == gate_arith)
      counter += apply_rules(gc, cur);
    stk.pop();
  }
}

}  // namespace

unsigned runHybridSimplifier(GenericCircuit &gc)
{
  unsigned counter = 0;
  std::unordered_set<gate_t> done;
  const auto nb = gc.getNbGates();
  for (std::size_t i = 0; i < nb; ++i) {
    simplify(gc, static_cast<gate_t>(i), done, counter);
  }
  return counter;
}

namespace {

/**
 * @brief Test whether both sides of @p cmp_gate are a continuous-only
 *        island (subtree of @c gate_value / @c gate_rv / @c gate_arith).
 *
 * A continuous island has no Boolean / aggregate / IO gates underneath
 * the cmp; the only outward edge is the cmp itself.  This is the
 * shape monteCarloRV's @c evalScalar can integrate over, so per-cmp
 * MC marginalisation is sound on these and these alone.
 */
bool is_continuous_island_cmp(const GenericCircuit &gc, gate_t cmp_gate)
{
  const auto &wires = gc.getWires(cmp_gate);
  if (wires.size() != 2) return false;

  std::unordered_set<gate_t> seen;
  std::stack<gate_t> stk;
  stk.push(wires[0]);
  stk.push(wires[1]);
  while (!stk.empty()) {
    gate_t g = stk.top(); stk.pop();
    if (!seen.insert(g).second) continue;
    auto t = gc.getGateType(g);
    if (t != gate_value && t != gate_rv && t != gate_arith) return false;
    for (gate_t c : gc.getWires(g)) stk.push(c);
  }
  return true;
}

/**
 * @brief Collect the base @c gate_rv leaves reachable from @p root
 *        through @c gate_arith composition.
 *
 * The set is the cmp's "RV footprint": two cmps share an island iff
 * their footprints overlap (a shared base RV is the only way their
 * sampled values can be correlated, given the island shape).
 */
void collect_cmp_rv_footprint(const GenericCircuit &gc, gate_t cmp_gate,
                              std::unordered_set<gate_t> &fp)
{
  std::unordered_set<gate_t> seen;
  std::stack<gate_t> stk;
  for (gate_t w : gc.getWires(cmp_gate)) stk.push(w);
  while (!stk.empty()) {
    gate_t g = stk.top(); stk.pop();
    if (!seen.insert(g).second) continue;
    auto t = gc.getGateType(g);
    if (t == gate_rv) { fp.insert(g); continue; }
    if (t == gate_arith) {
      for (gate_t c : gc.getWires(g)) stk.push(c);
    }
    /* gate_value contributes no RV identity; other types should not
     * appear here (is_continuous_island_cmp gates that path), but if
     * they did we'd simply ignore them in the footprint &ndash; the
     * decomposer's safety relies on the island-shape pre-check, not
     * on this routine. */
  }
}

}  // namespace

unsigned runHybridDecomposer(GenericCircuit &gc, unsigned samples)
{
  if (samples == 0) return 0;

  /* Snapshot all gate_cmp ids that look like continuous islands.
   * Each call later mutates a snapshot entry from @c gate_cmp to
   * @c gate_input via @c resolveCmpToBernoulli, but the snapshot
   * vector is unaffected.  The defensive type re-check at iteration
   * time guards against any pass between snapshot and resolution
   * having already mutated the gate. */
  const auto nb = gc.getNbGates();
  std::vector<gate_t> cmps;
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) == gate_cmp && is_continuous_island_cmp(gc, g))
      cmps.push_back(g);
  }

  /* Compute the per-cmp footprint up front so the pairwise-overlap
   * check is O(C * C * F) rather than O(C * C * tree_size). */
  std::unordered_map<gate_t, std::unordered_set<gate_t>> footprints;
  footprints.reserve(cmps.size());
  for (gate_t c : cmps) {
    collect_cmp_rv_footprint(gc, c, footprints[c]);
  }

  /* Identify cmps that share an island with another cmp (footprint
   * overlap on at least one base gate_rv).  These cannot be
   * marginalised independently: doing so would treat them as
   * spuriously independent.  The multi-cmp shared-island case is the
   * second half of Priority 7(b) and is intentionally skipped here. */
  std::unordered_set<gate_t> shared;
  for (std::size_t i = 0; i < cmps.size(); ++i) {
    for (std::size_t j = i + 1; j < cmps.size(); ++j) {
      const auto &fp_i = footprints[cmps[i]];
      const auto &fp_j = footprints[cmps[j]];
      /* Iterate the smaller set against the larger for the early-exit. */
      const auto &small = fp_i.size() < fp_j.size() ? fp_i : fp_j;
      const auto &big   = fp_i.size() < fp_j.size() ? fp_j : fp_i;
      for (gate_t rv : small) {
        if (big.count(rv)) {
          shared.insert(cmps[i]);
          shared.insert(cmps[j]);
          break;
        }
      }
    }
  }

  unsigned resolved = 0;
  for (gate_t c : cmps) {
    if (gc.getGateType(c) != gate_cmp) continue;
    if (shared.count(c)) continue;
    double p = monteCarloRV(gc, c, samples);
    gc.resolveCmpToBernoulli(c, p);
    ++resolved;
  }

  return resolved;
}

}  // namespace provsql
