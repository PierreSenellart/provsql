/**
 * @file HybridEvaluator.cpp
 * @brief Implementation of the peephole simplifier.
 *        See @c HybridEvaluator.h for the full docstring.
 */
#include "HybridEvaluator.h"

#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stack>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Aggregation.h"        // ComparisonOperator, cmpOpFromOid
#include "AnalyticEvaluator.h"  // cdfAt
#include "Expectation.h"        // evaluateBooleanProbability
#include "MonteCarloSampler.h"  // monteCarloRV, monteCarloScalarSamples
#include "RandomVariable.h"     // parse_distribution_spec, parseDoubleStrict, DistKind
extern "C" {
#include "provsql_utils.h"      // gate_type, provsql_arith_op
}
#include <algorithm>            // std::sort, std::unique, std::upper_bound

namespace provsql {

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/**
 * @brief Format a double back into the canonical text form used by
 *        @c gate_value extras.
 *
 * @c std::to_chars produces the shortest decimal representation that
 * round-trips through @c std::from_chars / @c std::stod, so round
 * cases like @c 0.2 = 0.4/2 print as @c "0.2" rather than
 * @c "0.20000000000000001" while irrational values fall back to
 * whatever length is needed for exact recovery.  The legacy
 * @c std::ostringstream @c << @c setprecision(17) path is kept as a
 * defensive fallback in case @c to_chars fails (range / buffer).
 *
 * @c std::ostringstream is used rather than @c std::snprintf in the
 * fallback because including @c <cstdio> after PostgreSQL's @c port.h
 * would expand @c std::snprintf to the non-existent
 * @c std::pg_snprintf via the @c #define snprintf macro.
 */
std::string double_to_text(double v)
{
  std::array<char, 32> buf;
  auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  if (ec == std::errc{}) return std::string(buf.data(), ptr);
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
 * @brief Rewrite @p g in place as a uniform @c gate_rv on @c [lo, hi].
 *
 * Used by the uniform-family closure (additive offset on a single
 * uniform term inside a PLUS, possibly with a negative scalar
 * coefficient that flips the support bounds) and by @c try_neg_rv
 * when @c -U(a,b) folds to @c U(-b,-a).
 *
 * Caller is responsible for ordering @c lo <= @c hi (we don't sort
 * defensively, so a swap-bounds bug elsewhere shows up immediately).
 */
void replace_with_uniform_rv(GenericCircuit &gc, gate_t g,
                             double lo, double hi)
{
  gc.resolveToRv(g, "uniform:" + double_to_text(lo)
                    + "," + double_to_text(hi));
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
 * @brief Try to interpret @p g as @c a*Z + b for a single base RV.
 *
 * Recognised shapes:
 * - bare @c gate_rv (any distribution):  @c (Z=g, a=1, b=0)
 * - bare @c gate_value:                  @c (Z=invalid, a=0, b=value)
 * - @c arith(NEG, child):                negate the child's decomposition
 * - @c arith(TIMES, value:c, child):     scale the child's decomposition
 *   by @c c (and symmetrically @c arith(TIMES, child, value:c)).
 *   Only 2-wire @c TIMES with exactly one @c gate_value side is
 *   recognised; other shapes fall through to "not decomposable".
 *
 * Nested @c arith(PLUS, ...) children of the outer PLUS are not
 * decomposed by this routine: the bottom-up simplifier already
 * folded them before the outer PLUS is processed, so by the time
 * we examine the outer PLUS its children are either leaves or
 * non-foldable arith.  An undecomposable wire causes the caller to
 * bail.
 *
 * Distribution-kind filtering is the caller's responsibility:
 * @c try_normal_closure additionally requires every @p rv_gate to be
 * @c DistKind::Normal, while @c try_plus_aggregate is kind-agnostic
 * because the aggregation rewrite preserves the base-RV identity.
 */
std::optional<LinearTerm>
decompose_linear_term(const GenericCircuit &gc, gate_t g)
{
  auto t = gc.getGateType(g);

  if (t == gate_value) {
    double v;
    try { v = parseDoubleStrict(gc.getExtra(g)); }
    catch (const CircuitException &) { return std::nullopt; }
    return LinearTerm{INVALID_GATE, 0.0, v};
  }

  if (t == gate_rv) {
    /* Any RV kind: aggregation only depends on identity, not on
     * closed-form scaling.  The normal-family closure filters to
     * @c DistKind::Normal externally. */
    return LinearTerm{g, 1.0, 0.0};
  }

  if (t == gate_mixture) {
    /* A @c gate_mixture (3-wire Bernoulli or categorical N-wire) is a
     * scalar-RV leaf: two references to the same @c gate_t produce
     * perfectly-correlated draws of the same RV.  Treat it like a
     * @c gate_rv so the PLUS aggregator can collapse same-mixture
     * terms (e.g. @c X+X to @c 2·X, @c X-X to @c 0).  The in-place
     * op-change to TIMES then triggers @c try_mixture_lift to push the
     * scalar inside the branches (3-wire) or the mulinputs'
     * value text (categorical).  The normal- and Erlang-family closures
     * filter on the rv leaf's kind via @c parse_distribution_spec, which
     * returns @c nullopt on a mixture's empty extra, so they
     * automatically bail when the LHS-RV side is a mixture. */
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
    return decompose_linear_term(gc, wires[0]);
  }

  if (op == PROVSQL_ARITH_NEG) {
    if (wires.size() != 1) return std::nullopt;
    auto inner = decompose_linear_term(gc, wires[0]);
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
    auto inner = decompose_linear_term(gc, var_side);
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
    auto term = decompose_linear_term(gc, w);
    if (!term) return false;
    /* The closure produces a single normal, so every non-constant
     * term's base RV must itself be normal.  The generic decomposer
     * does not filter by kind; we apply the filter here. */
    if (!is_invalid(term->rv_gate)) {
      auto spec = parse_distribution_spec(gc.getExtra(term->rv_gate));
      if (!spec || spec->kind != DistKind::Normal) return false;
    }
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
 * @brief Uniform-family closure on a @c PLUS gate.
 *
 * Fires when every wire decomposes (via @c decompose_linear_term) to
 * @c a*Z + b with at most one non-constant term whose @c gate_rv is a
 * Uniform.  The closure is @b not @c U + U: a sum of two distinct
 * uniforms is not uniform (it's a triangle / trapezoidal density), so
 * we bail when more than one Uniform term is present.  Any number of
 * pure-constant terms is fine (they collapse into a single additive
 * offset).
 *
 * For a single Uniform term <tt>a*U(p1, p2) + b_total</tt>:
 * - @c a > 0 ⇒ <tt>U(a*p1 + b_total, a*p2 + b_total)</tt>;
 * - @c a < 0 ⇒ <tt>U(a*p2 + b_total, a*p1 + b_total)</tt> (sign flip
 *   reorders the bounds).
 *
 * @c a == 0 is unreachable here: @c decompose_linear_term only assigns
 * @c a == 0 to pure-constant wires (where @c rv_gate is invalid), so a
 * Uniform-bearing term always has @c a != 0.  Same coupling caveat as
 * @c try_normal_closure: replacing @p g with a fresh @c gate_rv mints
 * a new RV identity, but @c try_plus_aggregate runs first and already
 * collapsed any shared-UUID U references, so by the time this rule
 * runs the surviving Uniform term has no sibling sharing its base RV.
 */
bool try_uniform_closure(GenericCircuit &gc, gate_t g)
{
  const auto &wires = gc.getWires(g);
  if (wires.size() < 2) return false;

  std::vector<LinearTerm> terms;
  terms.reserve(wires.size());
  for (gate_t w : wires) {
    auto term = decompose_linear_term(gc, w);
    if (!term) return false;
    if (!is_invalid(term->rv_gate)) {
      auto spec = parse_distribution_spec(gc.getExtra(term->rv_gate));
      if (!spec || spec->kind != DistKind::Uniform) return false;
    }
    terms.push_back(*term);
  }

  /* Exactly one Uniform term (U + U is not closed).  All other wires
   * must be pure constants.  We also need at least one Uniform
   * (otherwise the constant-fold path is responsible). */
  const LinearTerm *uniform = nullptr;
  for (const auto &t : terms) {
    if (is_invalid(t.rv_gate)) continue;
    if (uniform) return false;        /* second Uniform term */
    uniform = &t;
  }
  if (!uniform) return false;

  /* Sum every wire's additive offset.  The Uniform term's @c b is
   * an additive offset on the same wire as the RV; the closure
   * adds it to the global offset since (a*U + b_term) + offsets =
   * a*U + (b_term + offsets). */
  double b_total = 0.0;
  for (const auto &t : terms) b_total += t.b;

  auto spec = parse_distribution_spec(gc.getExtra(uniform->rv_gate));
  if (!spec || spec->kind != DistKind::Uniform) return false;
  const double a = uniform->a;
  const double p1 = spec->p1;
  const double p2 = spec->p2;
  const double new_lo = (a > 0.0) ? a * p1 + b_total : a * p2 + b_total;
  const double new_hi = (a > 0.0) ? a * p2 + b_total : a * p1 + b_total;

  replace_with_uniform_rv(gc, g, new_lo, new_hi);
  return true;
}

/**
 * @brief Negation closure on a bare @c gate_rv: rewrite @c arith(NEG, Z)
 *        as a closed-form-negated @c gate_rv when @c Z's family admits
 *        one.
 *
 * Supported families:
 * - <tt>-Normal(μ, σ) = Normal(-μ, σ)</tt> (sign flip on mean, σ ≥ 0
 *   unchanged).
 * - <tt>-Uniform(a, b) = Uniform(-b, -a)</tt> (sign flip reorders the
 *   bounds).
 *
 * Not closed (rule bails):
 * - <tt>-Exponential(λ)</tt>: support flips to @c (-∞, 0], no longer
 *   exponential.
 * - <tt>-Erlang(k, λ)</tt>: same support-flip issue.
 *
 * Coupling discipline: same as @c try_times_scalar_rv.  Pass-2 gated
 * so a parent PLUS containing @c NEG(Z) and a sibling reference to the
 * same @c Z is folded first by @c try_plus_aggregate (which recognises
 * @c NEG via @c decompose_linear_term's coefficient @c -1) before we
 * mint a fresh @c gate_rv at the NEG.
 */
bool try_neg_rv(GenericCircuit &gc, gate_t g)
{
  if (gc.getGateType(g) != gate_arith) return false;
  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  if (op != PROVSQL_ARITH_NEG) return false;
  const auto &wires = gc.getWires(g);
  if (wires.size() != 1) return false;
  if (gc.getGateType(wires[0]) != gate_rv) return false;

  auto spec = parse_distribution_spec(gc.getExtra(wires[0]));
  if (!spec) return false;

  switch (spec->kind) {
    case DistKind::Normal:
      replace_with_normal_rv(gc, g, -spec->p1, spec->p2);
      return true;
    case DistKind::Uniform:
      replace_with_uniform_rv(gc, g, -spec->p2, -spec->p1);
      return true;
    case DistKind::Exponential:
    case DistKind::Erlang:
      return false;
  }
  return false;
}

/**
 * @brief Mixture-lift rewrite: push @c PLUS / @c TIMES inside a
 *        single @c gate_mixture child.
 *
 * Fires on a @c gate_arith with op @c PLUS or @c TIMES whose children
 * contain exactly one @c gate_mixture.  Replaces the parent with a
 * @c gate_mixture sharing the same Bernoulli (so the original
 * <tt>p_token</tt> identity is preserved and any other gate that
 * referenced it continues to see it):
 *
 *   <tt>a + mixture(p, X, Y) → mixture(p, a + X, a + Y)</tt>
 *
 * The two new branches are fresh @c gate_arith children built via
 * @c addAnonymousArithGate; each is then re-fed to @c apply_rules so
 * the existing normal-family / erlang-family closures get a chance
 * to collapse them.  This is the source of the headline simplifier
 * gain for compound RV expressions: <tt>3 + mixture(p, N(0,1), N(2,1))</tt>
 * folds to <tt>mixture(p, N(3,1), N(5,1))</tt> in a single bottom-up
 * pass.
 *
 * Multi-mixture lifts (two or more @c gate_mixture children of the
 * same arith) are out of scope: each would multiply the branch count
 * by 2 and the lifted form would couple the resulting branches
 * through their Bernoullis, which the current closures cannot
 * collapse further.  @c MINUS / @c DIV / @c NEG lifts are also out of
 * scope (the user requested only @c PLUS and @c TIMES); they can be
 * added in a follow-up once the @c try_normal_closure handles
 * subtraction.
 *
 * Returns @c true if @p g was mutated.
 */
unsigned apply_rules(GenericCircuit &gc, gate_t g,
                     bool include_scalar_fold);  /* forward decl */

/**
 * @brief Categorical-mixture lift helper.
 *
 * Pushes a constant scaling (@c TIMES) or offset (@c PLUS) inside the
 * N-wire categorical-form @c gate_mixture <tt>[key, mul_1, ..., mul_n]</tt>
 * by minting a fresh categorical mixture sharing the same @p key gate
 * and one new @c gate_mulinput per outcome with an updated value text.
 *
 * Sharing the key preserves the semantic that the new mixture is a
 * deterministic function of the same underlying categorical draw (so
 * <tt>c · X</tt> and @c X stay perfectly correlated downstream via
 * FootprintCache key-overlap dependency tracking).  All other arith
 * wires must be @c gate_value constants; an RV factor / offset cannot
 * be pushed into a mulinput's scalar @c extra so the rule bails.
 *
 * Returns @c true if @p g was mutated.
 */
bool try_categorical_mixture_lift(GenericCircuit &gc, gate_t g,
                                  provsql_arith_op op,
                                  gate_t mix_gate,
                                  const std::vector<gate_t> &others)
{
  if (op != PROVSQL_ARITH_PLUS && op != PROVSQL_ARITH_TIMES) return false;

  /* Combine the non-mixture wires into a single scalar offset (PLUS)
   * or factor (TIMES).  Bail on any non-value wire: an RV factor /
   * offset cannot be pushed into a mulinput's value text. */
  double offset = 0.0;
  double factor = 1.0;
  for (gate_t w : others) {
    if (gc.getGateType(w) != gate_value) return false;
    double v;
    try { v = parseDoubleStrict(gc.getExtra(w)); }
    catch (const CircuitException &) { return false; }
    if (op == PROVSQL_ARITH_PLUS) offset += v;
    else                          factor *= v;
  }

  /* Build the new wire list: same key (preserves correlation with the
   * original categorical) and one fresh mulinput per outcome with the
   * transformed value text.  Snapshot the mixture's wires by value:
   * @c addAnonymousMulinputGateWithValue below calls @c addGate, which
   * does @c wires.push_back({}) on the circuit's outer wire vector,
   * and that can reallocate -- invalidating any reference returned by
   * @c getWires.  Reads of the reference after the first iteration
   * then return garbage gate ids, which surfaces either as wrong
   * outcome values or as a backend crash. */
  const std::vector<gate_t> mw = gc.getWires(mix_gate);
  const gate_t key = mw[0];
  std::vector<gate_t> new_wires;
  new_wires.reserve(mw.size());
  new_wires.push_back(key);
  for (std::size_t i = 1; i < mw.size(); ++i) {
    const gate_t old_mul = mw[i];
    double old_v;
    try { old_v = parseDoubleStrict(gc.getExtra(old_mul)); }
    catch (const CircuitException &) { return false; }
    const double new_v = (op == PROVSQL_ARITH_PLUS)
                         ? (offset + old_v)
                         : (factor * old_v);
    const double p = gc.getProb(old_mul);
    const auto vi = static_cast<unsigned>(gc.getInfos(old_mul).first);
    gate_t new_mul = gc.addAnonymousMulinputGateWithValue(
                       key, p, vi, double_to_text(new_v));
    new_wires.push_back(new_mul);
  }
  gc.resolveToCategoricalMixture(g, std::move(new_wires));
  return true;
}

bool try_mixture_lift(GenericCircuit &gc, gate_t g,
                      bool include_scalar_fold)
{
  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  if (op != PROVSQL_ARITH_PLUS && op != PROVSQL_ARITH_TIMES) return false;

  const auto &wires = gc.getWires(g);
  if (wires.size() < 2) return false;  /* nothing to lift */

  /* Find exactly one mixture child. */
  std::size_t mix_idx = static_cast<std::size_t>(-1);
  for (std::size_t i = 0; i < wires.size(); ++i) {
    if (gc.getGateType(wires[i]) == gate_mixture) {
      if (mix_idx != static_cast<std::size_t>(-1)) return false;
      mix_idx = i;
    }
  }
  if (mix_idx == static_cast<std::size_t>(-1)) return false;

  const auto mix_gate = wires[mix_idx];

  /* Snapshot the remaining wires.  We need a copy because the
   * resolveToMixture / resolveToCategoricalMixture calls below clear
   * the parent's wire vector. */
  std::vector<gate_t> others;
  others.reserve(wires.size() - 1);
  for (std::size_t i = 0; i < wires.size(); ++i) {
    if (i != mix_idx) others.push_back(wires[i]);
  }

  /* Categorical N-wire form: push the constant offset / factor into
   * each mulinput's value text.  RV factors / offsets cannot be pushed
   * into mulinput leaves so the rule bails on those. */
  if (gc.isCategoricalMixture(mix_gate)) {
    return try_categorical_mixture_lift(gc, g, op, mix_gate, others);
  }

  /* Classic 3-wire Bernoulli mixture. */
  const auto &mw = gc.getWires(mix_gate);
  if (mw.size() != 3) return false;
  const gate_t p_tok = mw[0];
  const gate_t x_tok = mw[1];
  const gate_t y_tok = mw[2];

  /* Build two new arith children: one with x in the mixture slot,
   * one with y.  Order matters for non-commutative ops, but PLUS /
   * TIMES are both commutative so we just append the branch RV to
   * the others. */
  std::vector<gate_t> new_x_wires = others; new_x_wires.push_back(x_tok);
  std::vector<gate_t> new_y_wires = others; new_y_wires.push_back(y_tok);
  gate_t new_x = gc.addAnonymousArithGate(op, std::move(new_x_wires));
  gate_t new_y = gc.addAnonymousArithGate(op, std::move(new_y_wires));

  /* Rewrite g as gate_mixture(p, new_x, new_y).  This clears g's
   * old wires / infos / extra and installs the new structure. */
  gc.resolveToMixture(g, p_tok, new_x, new_y);

  /* Recursively fold the two new arith children so they get a chance
   * to collapse via normal-family / erlang-family closures.  Each is
   * itself a gate_arith of the same op, with at least 2 wires (the
   * "others" we copied plus the branch RV), so apply_rules's
   * PLUS/TIMES path is the correct entry point.  The scalar-fold flag
   * is propagated so pass-2's scalar-times-RV closure stays the only
   * place that mints a fresh @c gate_rv at a scaled-RV TIMES site
   * (avoids losing shared-RV identity in front of a sibling PLUS). */
  apply_rules(gc, new_x, include_scalar_fold);
  apply_rules(gc, new_y, include_scalar_fold);

  return true;
}

/**
 * @brief Scalar-times-RV closure: fold @c arith(TIMES, value:c, Z) to
 *        a single closed-form-scaled @c gate_rv.
 *
 * Fires on a 2-wire @c TIMES whose wires are exactly one @c gate_value
 * (the scalar @c c) and one @c gate_rv leaf @c Z whose distribution
 * admits a closed-form scale transform:
 *
 * - <tt>c · Normal(μ, σ) = Normal(c·μ, |c|·σ)</tt> (any non-zero c).
 * - <tt>c · Uniform(a, b)</tt>: @c Uniform(c·a, c·b) for @c c > 0;
 *   @c Uniform(c·b, c·a) for @c c < 0.
 * - <tt>c · Exponential(λ) = Exponential(λ/c)</tt> for @c c > 0 only
 *   (negative scaling flips support to (-∞, 0] and is no longer
 *   exponential).
 * - <tt>c · Erlang(k, λ) = Erlang(k, λ/c)</tt> for @c c > 0 only.
 *
 * The c=0 absorber and c=1 identity are handled by
 * @c try_identity_drop, so this rule defensively bails on them to
 * avoid a duplicate rewrite path.  RV kinds without a closed-form
 * scaling fall through.
 *
 * Coupling caveat (shared with @c try_normal_closure): replacing the
 * TIMES with a fresh @c gate_rv mints a new RV identity at @p g, so
 * any other path that references @c Z and shares a downstream consumer
 * with @p g will see decoupled draws after the fold.  In practice the
 * rewrite path produces per-row orphan subtrees, so this is consistent
 * with the existing normal-family closure behaviour.
 *
 * Returns @c true if @p g was mutated.
 */
bool try_times_scalar_rv(GenericCircuit &gc, gate_t g)
{
  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  if (op != PROVSQL_ARITH_TIMES) return false;
  const auto &wires = gc.getWires(g);
  if (wires.size() != 2) return false;

  /* Identify the value side and the rv side. */
  double c = NaN;
  gate_t rv_side = INVALID_GATE;
  if (gc.getGateType(wires[0]) == gate_value
      && gc.getGateType(wires[1]) == gate_rv) {
    try { c = parseDoubleStrict(gc.getExtra(wires[0])); }
    catch (const CircuitException &) { return false; }
    rv_side = wires[1];
  } else if (gc.getGateType(wires[1]) == gate_value
             && gc.getGateType(wires[0]) == gate_rv) {
    try { c = parseDoubleStrict(gc.getExtra(wires[1])); }
    catch (const CircuitException &) { return false; }
    rv_side = wires[0];
  } else {
    return false;
  }

  /* c=0 / c=1 are the identity-drop's job; bailing here keeps the
   * two rules' responsibilities disjoint. */
  if (c == 0.0 || c == 1.0) return false;

  auto spec = parse_distribution_spec(gc.getExtra(rv_side));
  if (!spec) return false;

  switch (spec->kind) {
    case DistKind::Normal: {
      const double new_mu    = c * spec->p1;
      const double new_sigma = std::fabs(c) * spec->p2;
      /* Defensive: a zero-σ normal collapses to a Dirac.  σ=0 normals
       * are normally constructed via @c as_random by @c provsql.normal,
       * but if one slipped through (e.g. a future closure produced
       * σ=0 from the linear combination), route it through value. */
      if (new_sigma == 0.0) {
        replace_with_value(gc, g, new_mu);
      } else {
        replace_with_normal_rv(gc, g, new_mu, new_sigma);
      }
      return true;
    }
    case DistKind::Uniform: {
      const double a = spec->p1;
      const double b = spec->p2;
      const double lo = (c > 0.0) ? c * a : c * b;
      const double hi = (c > 0.0) ? c * b : c * a;
      replace_with_uniform_rv(gc, g, lo, hi);
      return true;
    }
    case DistKind::Exponential: {
      if (c <= 0.0) return false;
      const double new_lambda = spec->p1 / c;
      gc.resolveToRv(g, "exponential:" + double_to_text(new_lambda));
      return true;
    }
    case DistKind::Erlang: {
      if (c <= 0.0) return false;
      /* spec->p1 is integer-valued by construction (the SQL constructor
       * enforces k >= 1); guard defensively. */
      if (spec->p1 < 1.0 || spec->p1 != std::floor(spec->p1)) return false;
      const auto k = static_cast<unsigned long>(spec->p1);
      const double new_lambda = spec->p2 / c;
      replace_with_erlang_rv(gc, g, k, new_lambda);
      return true;
    }
  }
  return false;
}

/**
 * @brief PLUS coefficient aggregation: collapse same-base-RV terms
 *        in a sum.
 *
 * For a @c PLUS gate whose every wire decomposes via
 * @c decompose_linear_term to <tt>a·Z + b</tt>, sums the coefficients
 * per @c rv_gate UUID and accumulates all the constant offsets into a
 * single @c b_total.  Rebuilds the wire list as one @c TIMES per
 * surviving RV (or a bare RV wire when its coefficient is exactly @c 1)
 * plus a single @c value wire for @c b_total when non-zero.
 *
 * Fires when at least one of the following holds:
 *  - some @c rv_gate appears in more than one wire (the X+X case);
 *  - more than one constant wire is present (consolidates them).
 *
 * Without these triggers the rebuild would be a no-op or worse
 * (minting fresh @c TIMES wrappers identical in shape to existing
 * input wires), so the rule bails to keep the simplifier idempotent.
 *
 * Unlike @c try_normal_closure / @c try_times_scalar_rv, this rule is
 * @b safe under shared base-RV identity: the rebuild preserves every
 * @c rv_gate as a wire (wrapped in @c TIMES when its coefficient is
 * non-unit), so any other path that referenced @c Z continues to see
 * the same gate.  The subsequent fold of <tt>arith(TIMES, value:a, Z)</tt>
 * by @c try_times_scalar_rv inherits the same coupling caveat as the
 * existing normal-family closure (see its docstring).
 *
 * Returns @c true if @p g was mutated.
 */
bool try_plus_aggregate(GenericCircuit &gc, gate_t g,
                        bool include_scalar_fold)
{
  auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
  if (op != PROVSQL_ARITH_PLUS) return false;
  const auto &wires_in = gc.getWires(g);
  if (wires_in.size() < 2) return false;

  std::vector<LinearTerm> terms;
  terms.reserve(wires_in.size());
  for (gate_t w : wires_in) {
    auto t = decompose_linear_term(gc, w);
    if (!t) return false;
    terms.push_back(*t);
  }

  /* Aggregate per rv_gate.  A vector preserves insertion order so the
   * rebuilt wire list is deterministic across runs; the per-PLUS
   * arity is small enough that O(n²) lookup is fine. */
  std::vector<std::pair<gate_t, double>> coeffs;
  double b_total = 0.0;
  unsigned constants_in = 0;
  for (const auto &t : terms) {
    b_total += t.b;
    if (is_invalid(t.rv_gate)) {
      ++constants_in;
      continue;
    }
    bool found = false;
    for (auto &p : coeffs) {
      if (p.first == t.rv_gate) {
        p.second += t.a;
        found = true;
        break;
      }
    }
    if (!found) coeffs.emplace_back(t.rv_gate, t.a);
  }

  /* Fire only when there's actual consolidation to do.  Without a
   * duplicate RV (or multiple constants) the rebuild would mint
   * shape-equivalent TIMES wrappers for input wires like
   * arith(TIMES, value:a, Z), oscillating the gate vector. */
  const bool has_duplicate = (coeffs.size() < terms.size() - constants_in);
  const bool many_constants = (constants_in >= 2);
  if (!has_duplicate && !many_constants) return false;

  /* Drop zero-coefficient RVs (X + (-X) survivors). */
  std::vector<std::pair<gate_t, double>> kept;
  kept.reserve(coeffs.size());
  for (const auto &p : coeffs) {
    if (p.second != 0.0) kept.push_back(p);
  }

  /* All RVs canceled: fold g to a value gate carrying b_total. */
  if (kept.empty()) {
    replace_with_value(gc, g, b_total);
    return true;
  }

  /* Single surviving RV term with no constant offset.  Rewrite g
   * directly in place as the simplest representation:
   *  - a == 1 ⇒ singleton PLUS([Z]) (we can't safely dissolve to Z
   *    in place because that would mint a fresh RV identity at g).
   *  - a != 1 ⇒ in-place op-change from PLUS to TIMES with wires
   *    [value:a, Z].  When @p include_scalar_fold is set the fixed-point
   *    loop then re-enters apply_rules on g (now a TIMES), giving
   *    try_times_scalar_rv a chance to fold the scaled RV.  Pass 1
   *    runs with @p include_scalar_fold = false (deferring the fold so
   *    the outer aggregator sees @c c·X-shaped children with intact
   *    RV identity); pass 2 then folds the surviving TIMES wrapper.
   *    Either way, the in-place op-change avoids the PLUS([TIMES(..)])
   *    double wrapper that would otherwise hide the bare-RV shape from
   *    @c AnalyticEvaluator's @c bareRv lookup. */
  if (kept.size() == 1 && b_total == 0.0) {
    const auto &only = kept.front();
    if (only.second == 1.0) {
      gc.setWires(g, {only.first});
    } else {
      const gate_t cv = gc.addAnonymousValueGate(
                          double_to_text(only.second));
      gc.setInfos(g, static_cast<unsigned>(PROVSQL_ARITH_TIMES), 0);
      gc.setWires(g, {cv, only.first});
    }
    return true;
  }

  /* General case: rebuild g as a multi-wire PLUS. */
  std::vector<gate_t> new_wires;
  new_wires.reserve(kept.size() + 1);
  for (const auto &p : kept) {
    if (p.second == 1.0) {
      new_wires.push_back(p.first);
    } else {
      const gate_t cv = gc.addAnonymousValueGate(double_to_text(p.second));
      const gate_t tm = gc.addAnonymousArithGate(PROVSQL_ARITH_TIMES,
                                                  {cv, p.first});
      new_wires.push_back(tm);
    }
  }
  if (b_total != 0.0) {
    new_wires.push_back(gc.addAnonymousValueGate(double_to_text(b_total)));
  }

  gc.setWires(g, std::move(new_wires));

  /* Recurse into freshly-minted TIMES children so try_times_scalar_rv
   * gets a chance to fold them within the same bottom-up pass when
   * @p include_scalar_fold is set.  Same pattern as try_mixture_lift. */
  for (gate_t w : gc.getWires(g)) {
    if (gc.getGateType(w) == gate_arith) {
      apply_rules(gc, w, include_scalar_fold);
    }
  }
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
unsigned apply_rules(GenericCircuit &gc, gate_t g,
                     bool include_scalar_fold)
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

    /* 1b. MINUS-to-PLUS canonicalisation.  Rewrites
     *     @c arith(MINUS, A, B) as @c arith(PLUS, A, arith(NEG, B))
     *     so every downstream rule -- PLUS aggregation, family
     *     closures, mixture-lift -- only needs to handle PLUS.
     *     @c decompose_linear_term already recognises @c NEG as a
     *     coefficient @c -1, so the rewritten parent's
     *     @c decompose_linear_term yields the same linear-term shape
     *     as the original MINUS would have, modulo one extra
     *     gate_arith level for the NEG.  Runs after constant fold so
     *     a fully-constant @c MINUS(value, value) collapses to a
     *     @c value gate without minting an interim NEG. */
    {
      auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      if (op == PROVSQL_ARITH_MINUS) {
        const auto &wires_in = gc.getWires(g);
        if (wires_in.size() == 2) {
          const gate_t a = wires_in[0];
          const gate_t b = wires_in[1];
          const gate_t neg_b = gc.addAnonymousArithGate(PROVSQL_ARITH_NEG,
                                                        {b});
          gc.setInfos(g, static_cast<unsigned>(PROVSQL_ARITH_PLUS), 0);
          gc.setWires(g, {a, neg_b});
          ++local;
          continue;
        }
      }
    }

    /* 1c. DIV-by-constant to TIMES-by-reciprocal canonicalisation.
     *     Rewrites @c arith(DIV, X, value:c) as
     *     @c arith(TIMES, X, value:1/c) (c != 0) so the existing
     *     scalar-times-RV closure (@c try_times_scalar_rv) and every
     *     other downstream TIMES rule fold @c X/c uniformly with
     *     @c c*X.  DIV-by-non-constant is left alone (no closure to
     *     apply); fully-constant @c DIV(value, value) is handled by
     *     the constant fold above so we never see @c c=0 here. */
    {
      auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      if (op == PROVSQL_ARITH_DIV) {
        const auto &wires_in = gc.getWires(g);
        if (wires_in.size() == 2) {
          const double c = try_eval_constant(gc, wires_in[1]);
          if (!std::isnan(c) && c != 0.0) {
            const gate_t x = wires_in[0];
            const gate_t inv = gc.addAnonymousValueGate(
                                  double_to_text(1.0 / c));
            gc.setInfos(g, static_cast<unsigned>(PROVSQL_ARITH_TIMES), 0);
            gc.setWires(g, {x, inv});
            ++local;
            continue;
          }
        }
      }
    }

    /* 2. Identity / absorber drops on PLUS and TIMES. */
    if (try_identity_drop(gc, g)) {
      ++local;
      continue;
    }

    /* 3. Mixture lift: push PLUS / TIMES inside a single mixture
     *    child.  Runs BEFORE the normal / erlang closures so the
     *    branch arith children get to try those closures themselves
     *    after the lift.  Once the lift fires the parent is no
     *    longer gate_arith, so the loop terminates on the next
     *    iteration via the gate_arith guard above. */
    if (try_mixture_lift(gc, g, include_scalar_fold)) {
      ++local;
      break;
    }

    auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);

    /* 4. PLUS coefficient aggregation: collapse X+X, X-X, multiple
     *    constants, etc.  Runs BEFORE the family closures so they see
     *    a sum with distinct RV identities (which they assume), and
     *    so X+X folds through the scalar-times-RV closure on the
     *    minted 2*X child. */
    if (op == PROVSQL_ARITH_PLUS) {
      if (try_plus_aggregate(gc, g, include_scalar_fold)) {
        ++local;
        continue;
      }
    }

    /* 5. Scalar-times-RV closure on TIMES: c · gate_rv folds to a
     *    closed-form-scaled gate_rv for the supported families.  Gated
     *    by @p include_scalar_fold: the bottom-up DFS visits children
     *    before parents, and folding @c c·X to a fresh @c gate_rv at
     *    the TIMES gate would lose @c X's identity, which an outer
     *    @c PLUS-aggregation sibling like @c x in @c 2·x+x relies on
     *    to recognise the shared base RV.  Pass 1 runs all other rules
     *    so the aggregator gets first crack at @c c·X-shaped wires;
     *    pass 2 then folds the remaining TIMES gates with this rule
     *    via @c runHybridSimplifier's post-pass. */
    if (op == PROVSQL_ARITH_TIMES && include_scalar_fold) {
      if (try_times_scalar_rv(gc, g)) {
        ++local;
        break;
      }
    }

    /* 6. Family closures on PLUS.  Order:
     *      - normal (handles every-wire-normal sums);
     *      - erlang (handles every-wire-exp/erlang same-rate sums);
     *      - uniform (handles at-most-one-Uniform + pure-constant
     *        sums, including the post-MINUS-canonicalisation shapes
     *        @c c + (-U) and @c (-U) + c).
     *    The three families are mutually exclusive on the underlying
     *    spec (a Uniform-bearing wire fails the normal- and Erlang-
     *    closure filters), so order does not matter for correctness;
     *    we keep the historical order for the first two and append
     *    the new rule at the end. */
    if (op == PROVSQL_ARITH_PLUS) {
      if (try_normal_closure(gc, g))  { ++local; break; }
      if (try_erlang_closure(gc, g))  { ++local; break; }
      if (try_uniform_closure(gc, g)) { ++local; break; }
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
              std::unordered_set<gate_t> &done, unsigned &counter,
              bool include_scalar_fold)
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
      counter += apply_rules(gc, cur, include_scalar_fold);
    stk.pop();
  }
}

}  // namespace

unsigned runConstantFold(GenericCircuit &gc)
{
  unsigned counter = 0;
  /* Walk every gate in order: @c try_eval_constant recurses through
   * @c gate_arith children itself (via @c try_eval_constant's own
   * recursion on @c gate_arith ops + base case at @c gate_value),
   * so a single linear pass over the gate indices is sufficient.
   * No DFS bookkeeping needed because the rewrite produces a
   * @c gate_value (terminal), never another @c gate_arith. */
  const auto nb = gc.getNbGates();
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    if (gc.getGateType(g) != gate_arith) continue;
    double c = try_eval_constant(gc, g);
    if (!std::isnan(c)) {
      replace_with_value(gc, g, c);
      ++counter;
    }
  }
  return counter;
}

unsigned runHybridSimplifier(GenericCircuit &gc)
{
  unsigned counter = 0;

  /* Pass 1: bottom-up DFS applying every rule EXCEPT the scalar-times-RV
   * fold.  Deferring that one rule lets @c try_plus_aggregate see
   * @c arith(TIMES, value:c, X) shapes inside a parent PLUS -- the
   * decomposer recognises them as @c c·X with @c rv_gate=X, so a
   * sibling @c x in @c 2·x + x correctly aggregates to coefficient
   * three on the shared base RV.  If the scalar fold had fired bottom-up
   * on the inner TIMES first it would have minted a fresh @c gate_rv
   * there, decoupling its identity from the sibling @c x and forcing
   * the outer normal-closure path which assumes independence. */
  {
    std::unordered_set<gate_t> done;
    const auto nb = gc.getNbGates();
    for (std::size_t i = 0; i < nb; ++i) {
      simplify(gc, static_cast<gate_t>(i), done, counter,
               /*include_scalar_fold=*/false);
    }
  }

  /* Pass 2: scalar-times-RV fold and NEG-of-RV fold on every
   * remaining @c gate_arith.  Pass 1's aggregator and family closures
   * have already consumed the shapes where these folds would have
   * lost shared-RV identity; any surviving 2-wire
   * <tt>arith(TIMES, value:c, gate_rv)</tt> or 1-wire
   * <tt>arith(NEG, gate_rv)</tt> is now either standalone (no sibling
   * to couple with) or the leftover wrapper from a single-RV
   * aggregation result.  No DFS is needed -- the rules are local and
   * idempotent, and walking the gate range with the post-pass-1
   * @c getNbGates() picks up the freshly minted wrappers from
   * @c try_plus_aggregate, @c try_mixture_lift, and the
   * MINUS-to-PLUS canonicalisation. */
  {
    const auto nb = gc.getNbGates();
    for (std::size_t i = 0; i < nb; ++i) {
      auto g = static_cast<gate_t>(i);
      if (gc.getGateType(g) == gate_arith) {
        if (try_times_scalar_rv(gc, g)) ++counter;
        else if (try_neg_rv(gc, g))     ++counter;
      }
    }
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
    if (t == gate_value || t == gate_rv || t == gate_arith) {
      for (gate_t c : gc.getWires(g)) stk.push(c);
      continue;
    }
    if (t == gate_mixture) {
      /* Categorical-form mixture (from @c provsql.categorical): a
       * discrete scalar leaf with no continuous identities below.
       * Treat it as a black-box scalar leaf and don't descend. */
      if (gc.isCategoricalMixture(g)) continue;
      /* Classic 3-wire mixture: first wire is a gate_input Bernoulli;
       * the rest of the island walker would reject it as
       * non-continuous, but the Monte-Carlo sampler handles it
       * correctly via per-iteration coupling.  Treat the mixture as
       * a black-box scalar leaf in the island shape: do NOT descend
       * into wires[0], only into the scalar branches wires[1] /
       * wires[2]. */
      const auto &mw = gc.getWires(g);
      if (mw.size() != 3) return false;
      stk.push(mw[1]);
      stk.push(mw[2]);
      continue;
    }
    return false;
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
      continue;
    }
    if (t == gate_mixture) {
      /* Categorical-form mixture (from @c provsql.categorical):
       * discrete leaves, no continuous identities below.  Stop. */
      if (gc.isCategoricalMixture(g)) continue;
      /* Classic 3-wire mixture: descend into the scalar branches but
       * NOT into the Bernoulli (wires[0] is a gate_input, not a
       * continuous RV identity).  Two cmps that share a mixture's
       * continuous RVs still need to be grouped together; sharing the
       * Bernoulli alone does too, but that coupling is captured at
       * the sampler level rather than here -- the joint-table sampler
       * hits both cmps in the same MC iteration and the shared
       * bool_cache_ produces coherent draws. */
      const auto &mw = gc.getWires(g);
      if (mw.size() == 3) { stk.push(mw[1]); stk.push(mw[2]); }
      continue;
    }
    /* gate_value contributes no RV identity; other types should not
     * appear here (is_continuous_island_cmp gates that path), but if
     * they did we'd simply ignore them in the footprint &ndash; the
     * decomposer's safety relies on the island-shape pre-check, not
     * on this routine. */
  }
}

}  // namespace

namespace {

/* Joint-table cap.  2^k mulinput leaves are materialised per group;
 * 256 cells is more than ample for HAVING/WHERE workloads while
 * keeping the in-memory footprint and the per-cell MC variance
 * (samples / 2^k counts per cell) bounded.  Groups exceeding the
 * cap fall through to whole-circuit MC by leaving their cmps as
 * gate_cmp; the dispatch in probability_evaluate then routes
 * through monteCarloRV. */
constexpr std::size_t JOINT_TABLE_K_MAX = 8;

/**
 * @brief Test whether @c AnalyticEvaluator would resolve @p cmp_gate
 *        analytically on its own.
 *
 * The decomposer now runs before @c AnalyticEvaluator (so shared
 * bare-RV cmps reach the grouping logic and the fast path's
 * analytical CDF can fire), but it must leave isolated bare-RV cmps
 * untouched: marginalising those via MC would waste samples on a
 * case the closed-form CDF handles exactly.  Mirror the shape match
 * in @c tryAnalyticDecide (bare RV vs gate_value either way around;
 * two bare normal RVs).
 */
bool is_analytic_singleton_cmp(const GenericCircuit &gc, gate_t cmp_gate)
{
  const auto &wires = gc.getWires(cmp_gate);
  if (wires.size() != 2) return false;
  auto t0 = gc.getGateType(wires[0]);
  auto t1 = gc.getGateType(wires[1]);

  /* X cmp c / c cmp X: AnalyticEvaluator resolves any supported
   * distribution kind via the closed-form CDF. */
  if ((t0 == gate_rv && t1 == gate_value) ||
      (t0 == gate_value && t1 == gate_rv))
    return true;

  /* Categorical-form mixture cmp constant: AnalyticEvaluator's
   * @c categoricalDecide computes the exact mass sum over the
   * mulinputs satisfying the predicate, so the decomposer should not
   * pre-empt with per-cmp MC.  Also picks up the
   * @c try_categorical_mixture_lift output (a constant scaled / offset
   * categorical), keeping the analytical path end-to-end for
   * <tt>c · X cmp k</tt> shapes over categorical RVs. */
  if ((gc.isCategoricalMixture(wires[0]) && t1 == gate_value) ||
      (gc.isCategoricalMixture(wires[1]) && t0 == gate_value))
    return true;

  /* X cmp Y both bare normals: AnalyticEvaluator's normal-diff
   * shortcut applies. */
  if (t0 == gate_rv && t1 == gate_rv) {
    auto sx = parse_distribution_spec(gc.getExtra(wires[0]));
    auto sy = parse_distribution_spec(gc.getExtra(wires[1]));
    if (sx && sy && sx->kind == DistKind::Normal
                 && sy->kind == DistKind::Normal)
      return true;
  }
  return false;
}

/**
 * @brief Information needed by @c inline_fast_path: the shared scalar
 *        plus, for each cmp, the comparison operator and the
 *        constant rhs threshold (after flipping for cmps shaped
 *        @c c @c op @c X).
 */
struct FastPathInfo {
  gate_t scalar;
  std::vector<ComparisonOperator> ops;     /* one per cmp, oriented as `scalar op c` */
  std::vector<double> thresholds;          /* one per cmp */
};

ComparisonOperator flip_cmp_op(ComparisonOperator op)
{
  switch (op) {
    case ComparisonOperator::LT: return ComparisonOperator::GT;
    case ComparisonOperator::LE: return ComparisonOperator::GE;
    case ComparisonOperator::GT: return ComparisonOperator::LT;
    case ComparisonOperator::GE: return ComparisonOperator::LE;
    case ComparisonOperator::EQ: return ComparisonOperator::EQ;
    case ComparisonOperator::NE: return ComparisonOperator::NE;
  }
  return op;
}

bool apply_cmp(double l, ComparisonOperator op, double r)
{
  switch (op) {
    case ComparisonOperator::LT: return l <  r;
    case ComparisonOperator::LE: return l <= r;
    case ComparisonOperator::EQ: return l == r;
    case ComparisonOperator::NE: return l != r;
    case ComparisonOperator::GE: return l >= r;
    case ComparisonOperator::GT: return l >  r;
  }
  return false;
}

/**
 * @brief Detect the monotone-shared-scalar fast path on a group of
 *        comparators.
 *
 * Fires when every cmp in @p cmps has one side equal to a single
 * shared gate_t @c s and the other side a @c gate_value: the k cmps
 * then jointly partition the @c s-line into at most k+1 intervals,
 * with each interval producing a deterministic k-bit outcome.  This
 * shape is common in HAVING / WHERE with multiple thresholds on the
 * same aggregate / column: e.g.
 * <tt>count(*) > 10 OR count(*) < 5</tt>.
 *
 * Returns @c std::nullopt when any cmp has both non-constant sides,
 * when the cmps don't all share the same @c s gate_t, when a
 * comparator OID is unrecognised, or when @c EQ / @c NE appears (the
 * interval representation can't express a measure-zero point).
 */
std::optional<FastPathInfo>
detect_shared_scalar(const GenericCircuit &gc,
                     const std::vector<gate_t> &cmps)
{
  FastPathInfo info;
  info.ops.reserve(cmps.size());
  info.thresholds.reserve(cmps.size());
  bool first = true;

  for (gate_t c : cmps) {
    const auto &wires = gc.getWires(c);
    if (wires.size() != 2) return std::nullopt;

    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(c).first, ok);
    if (!ok) return std::nullopt;
    /* EQ / NE on continuous RVs have measure zero / one and were
     * already resolved by RangeCheck; if we still see one we don't
     * know how to fit it into an interval partition.  Bail. */
    if (op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
      return std::nullopt;

    gate_t scalar_side = static_cast<gate_t>(-1);
    double threshold = std::numeric_limits<double>::quiet_NaN();
    ComparisonOperator effective_op = op;
    if (gc.getGateType(wires[1]) == gate_value) {
      scalar_side = wires[0];
      try { threshold = parseDoubleStrict(gc.getExtra(wires[1])); }
      catch (const CircuitException &) { return std::nullopt; }
    } else if (gc.getGateType(wires[0]) == gate_value) {
      scalar_side = wires[1];
      try { threshold = parseDoubleStrict(gc.getExtra(wires[0])); }
      catch (const CircuitException &) { return std::nullopt; }
      effective_op = flip_cmp_op(op);
    } else {
      return std::nullopt;
    }

    if (first) {
      info.scalar = scalar_side;
      first = false;
    } else if (info.scalar != scalar_side) {
      return std::nullopt;
    }
    info.ops.push_back(effective_op);
    info.thresholds.push_back(threshold);
  }
  return info;
}

/**
 * @brief Inline a fast-path joint table for a monotone-shared-scalar
 *        group.
 *
 * The k cmps partition the scalar line into at most k+1 intervals
 * (one per pair of consecutive sorted distinct thresholds plus the
 * two infinite tails).  Each interval gets a single mulinput with
 * probability equal to the scalar's mass on the interval; the
 * comparator outcomes are deterministic per interval (evaluated at
 * a strictly-interior representative point) and the k cmps are
 * rewritten as @c gate_plus over the mulinputs whose interval makes
 * them true.
 *
 * Interval probabilities are computed analytically via @c cdfAt when
 * the scalar is a bare @c gate_rv with a CDF the helper supports;
 * otherwise (a @c gate_arith composite, or an Erlang with
 * non-integer shape) we fall back to MC by sampling the scalar
 * @p samples times and binning into intervals.
 */
void inline_fast_path(GenericCircuit &gc,
                      const std::vector<gate_t> &cmps,
                      const FastPathInfo &info,
                      unsigned samples)
{
  /* Sort + dedup thresholds; the resulting m distinct boundaries
   * partition R into m+1 open intervals
   * (-∞, t_0), (t_0, t_1), ..., (t_{m-1}, +∞). */
  std::vector<double> ts = info.thresholds;
  std::sort(ts.begin(), ts.end());
  ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
  const std::size_t m = ts.size();
  const std::size_t nb_intervals = m + 1;

  /* Compute interval probabilities.  Try the analytical CDF first:
   * when the shared scalar is a bare @c gate_rv with a CDF
   * @c cdfAt understands, the interval probability is
   * @c F(t_{i+1}) - F(t_i) exactly &mdash; no MC noise, no sampling.
   * This is the headline benefit of the fast path: shared bare-RV
   * groups land on the exact dependent truth and the resulting
   * Bernoulli probabilities propagate through tree-decomposition /
   * compilation without any sampling noise contributed by the
   * decomposer.  Fall back to MC binning over @p samples scalar
   * draws when the scalar is a @c gate_arith composite (no CDF) or
   * when @c cdfAt returns NaN on a boundary (Erlang with
   * non-integer shape, etc.). */
  std::vector<double> interval_probs(nb_intervals, 0.0);
  bool analytical = false;
  if (gc.getGateType(info.scalar) == gate_rv) {
    auto spec = parse_distribution_spec(gc.getExtra(info.scalar));
    if (spec) {
      std::vector<double> cdf_at_boundary(m);
      bool all_ok = true;
      for (std::size_t i = 0; i < m; ++i) {
        cdf_at_boundary[i] = cdfAt(*spec, ts[i]);
        if (std::isnan(cdf_at_boundary[i])) { all_ok = false; break; }
      }
      if (all_ok) {
        interval_probs[0] = cdf_at_boundary[0];
        for (std::size_t i = 1; i < m; ++i)
          interval_probs[i] = cdf_at_boundary[i] - cdf_at_boundary[i - 1];
        interval_probs[m] = 1.0 - cdf_at_boundary[m - 1];
        analytical = true;
      }
    }
  }
  if (!analytical) {
    auto draws = monteCarloScalarSamples(gc, info.scalar, samples);
    for (double s : draws) {
      auto it = std::upper_bound(ts.begin(), ts.end(), s);
      std::size_t idx = static_cast<std::size_t>(it - ts.begin());
      ++interval_probs[idx];
    }
    for (auto &p : interval_probs) p /= samples;
  }

  /* For each interval, determine the k-bit cmp outcome word.  Pick
   * a representative point strictly inside the interval: the
   * midpoint for finite intervals, t_0 - 1 / t_{m-1} + 1 for the
   * infinite tails.  Continuous distributions assign zero mass to
   * the boundaries, so the choice of interior point doesn't
   * affect any cmp's outcome on the open interval. */
  std::vector<unsigned long> outcome_word(nb_intervals, 0);
  for (std::size_t i = 0; i < nb_intervals; ++i) {
    double point;
    if (i == 0)              point = ts[0] - 1.0;
    else if (i == m)         point = ts[m - 1] + 1.0;
    else                     point = 0.5 * (ts[i - 1] + ts[i]);
    unsigned long w = 0;
    for (std::size_t j = 0; j < info.thresholds.size(); ++j) {
      if (apply_cmp(point, info.ops[j], info.thresholds[j]))
        w |= (1ul << j);
    }
    outcome_word[i] = w;
  }

  /* Allocate key + per-interval mulinputs (skipping zero-prob
   * intervals to keep the materialised circuit lean). */
  gate_t key = gc.addAnonymousInputGate(1.0);
  std::vector<gate_t> mul_for_interval(nb_intervals,
                                       static_cast<gate_t>(-1));
  for (std::size_t i = 0; i < nb_intervals; ++i) {
    if (interval_probs[i] <= 0.0) continue;
    mul_for_interval[i] =
      gc.addAnonymousMulinputGate(key, interval_probs[i],
                                  static_cast<unsigned>(i));
  }

  /* Rewrite each cmp as gate_plus over the mulinputs whose
   * interval-outcome word has the cmp's bit set. */
  for (std::size_t j = 0; j < cmps.size(); ++j) {
    std::vector<gate_t> plus_wires;
    plus_wires.reserve(nb_intervals);
    for (std::size_t i = 0; i < nb_intervals; ++i) {
      if (!(outcome_word[i] & (1ul << j))) continue;
      gate_t mw = mul_for_interval[i];
      if (mw == static_cast<gate_t>(-1)) continue;
      plus_wires.push_back(mw);
    }
    gc.resolveToPlus(cmps[j], std::move(plus_wires));
  }
}

/**
 * @brief Inline a joint-distribution table over a group of k cmps
 *        sharing an island.
 *
 * Materialises 2^k - z mulinput leaves (where z is the number of
 * outcomes with empirical probability zero, omitted to keep the
 * circuit lean), all sharing a fresh anonymous key gate.  Each
 * comparator @c cmps[i] is rewritten in place as @c gate_plus over
 * the mulinputs whose joint outcome word has bit @c i set; the
 * combined probability is the marginal P(cmp_i = 1) and shared bits
 * across different cmps reuse the same mulinput leaf so the OR over
 * cmps at downstream sites correctly observes the joint distribution
 * (mutually exclusive over the joint outcomes).
 *
 * Sound when the per-iteration sampler memoisation in
 * @c monteCarloRV / @c monteCarloJointDistribution gives all k cmps
 * a consistent draw of the shared island - which is precisely the
 * is_continuous_island_cmp + shared-footprint precondition the
 * caller has already enforced.
 */
void inline_joint_table(GenericCircuit &gc,
                        const std::vector<gate_t> &cmps,
                        unsigned samples)
{
  const unsigned k = static_cast<unsigned>(cmps.size());
  auto probs = monteCarloJointDistribution(gc, cmps, samples);

  /* Fresh key gate (the anonymous block anchor for these mulinputs).
   * Probability 1.0 because the key itself is not a sampled choice;
   * the mutually-exclusive outcomes among the mulinputs are what
   * carries the joint mass. */
  gate_t key = gc.addAnonymousInputGate(1.0);

  /* Allocate one mulinput per joint outcome with positive probability.
   * Zero-probability outcomes are pruned: the cmp gate_plus
   * rewrites below would have included them as wires with prob 0,
   * which is a no-op in OR (gate_zero is the additive identity).
   * value_index = w gives independentEvaluation's mulin_seen dedup
   * a stable key (group, info) per outcome. */
  const std::size_t nb_outcomes = std::size_t{1} << k;
  std::vector<gate_t> mul_for_outcome(nb_outcomes,
                                      static_cast<gate_t>(-1));
  for (std::size_t w = 0; w < nb_outcomes; ++w) {
    if (probs[w] <= 0.0) continue;
    mul_for_outcome[w] =
      gc.addAnonymousMulinputGate(key, probs[w],
                                  static_cast<unsigned>(w));
  }

  /* Rewrite each cmp as gate_plus over the mulinputs whose joint
   * outcome word has the cmp's bit set. */
  for (unsigned i = 0; i < k; ++i) {
    std::vector<gate_t> plus_wires;
    plus_wires.reserve(nb_outcomes / 2);
    for (std::size_t w = 0; w < nb_outcomes; ++w) {
      if ((w & (std::size_t{1} << i)) == 0) continue;
      gate_t m = mul_for_outcome[w];
      if (m == static_cast<gate_t>(-1)) continue;
      plus_wires.push_back(m);
    }
    gc.resolveToPlus(cmps[i], std::move(plus_wires));
  }
}

}  // namespace

unsigned runHybridDecomposer(GenericCircuit &gc, unsigned samples)
{
  if (samples == 0) return 0;

  /* Snapshot all gate_cmp ids that look like continuous islands.
   * Each call later mutates a snapshot entry from @c gate_cmp to
   * @c gate_input via @c resolveCmpToBernoulli (singleton group)
   * or to @c gate_plus via @c resolveToPlus (multi-cmp group), but
   * the snapshot vector is unaffected.  The defensive type re-check
   * at iteration time guards against intervening mutations. */
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

  /* Group cmps into connected components by base-RV footprint
   * overlap (union-find via parent[]).  Linear-probe path
   * compression keeps the asymptotics near-linear in the number of
   * pairwise overlap checks. */
  std::vector<std::size_t> parent(cmps.size());
  for (std::size_t i = 0; i < cmps.size(); ++i) parent[i] = i;
  auto find = [&](std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  auto unite = [&](std::size_t a, std::size_t b) {
    a = find(a); b = find(b);
    if (a != b) parent[a] = b;
  };
  for (std::size_t i = 0; i < cmps.size(); ++i) {
    for (std::size_t j = i + 1; j < cmps.size(); ++j) {
      if (find(i) == find(j)) continue;
      const auto &fp_i = footprints[cmps[i]];
      const auto &fp_j = footprints[cmps[j]];
      const auto &small = fp_i.size() < fp_j.size() ? fp_i : fp_j;
      const auto &big   = fp_i.size() < fp_j.size() ? fp_j : fp_i;
      for (gate_t rv : small) {
        if (big.count(rv)) { unite(i, j); break; }
      }
    }
  }

  /* Collect cmps by component root. */
  std::unordered_map<std::size_t, std::vector<gate_t>> groups;
  for (std::size_t i = 0; i < cmps.size(); ++i)
    groups[find(i)].push_back(cmps[i]);

  unsigned resolved = 0;
  for (auto &[root, group] : groups) {
    (void) root;
    /* Defensive: re-check every cmp is still gate_cmp.  Nothing in
     * the pipeline should have mutated them since the snapshot, but
     * the check is cheap. */
    bool all_pristine = true;
    for (gate_t c : group) {
      if (gc.getGateType(c) != gate_cmp) { all_pristine = false; break; }
    }
    if (!all_pristine) continue;

    if (group.size() == 1) {
      /* Singleton island.  If AnalyticEvaluator would resolve this
       * cmp exactly on its own (bare gate_rv vs gate_value, or two
       * bare normals), leave it untouched and let the closed-form
       * pass below handle it - no point burning MC samples on a
       * case with an analytical answer.  Otherwise MC-marginalise
       * into a Bernoulli leaf here. */
      if (is_analytic_singleton_cmp(gc, group[0])) continue;
      double p = monteCarloRV(gc, group[0], samples);
      gc.resolveCmpToBernoulli(group[0], p);
      ++resolved;
      continue;
    }

    /* Multi-cmp shared island.  Try the monotone-shared-scalar fast
     * path first: when every cmp has shape `s op c` for a common
     * scalar gate_t s, the joint table is built from k+1 intervals
     * (analytical when s is a bare gate_rv with a known CDF, MC
     * binning otherwise) instead of 2^k cells, and the test
     * 14-style shared bare-RV case (`X > 0 OR X > 1`) lands on the
     * exact answer with no MC noise.  When detection fails, fall
     * through to the generic 2^k MC joint table iff k is small
     * enough; larger groups keep their cmps as gate_cmp and fall
     * through to whole-circuit MC. */
    if (auto info = detect_shared_scalar(gc, group)) {
      inline_fast_path(gc, group, *info, samples);
      resolved += static_cast<unsigned>(group.size());
      continue;
    }

    if (group.size() > JOINT_TABLE_K_MAX) continue;

    inline_joint_table(gc, group, samples);
    resolved += static_cast<unsigned>(group.size());
  }

  return resolved;
}

}  // namespace provsql
