/**
 * @file Expectation.cpp
 * @brief Implementation of the analytical expectation / variance / moment
 *        evaluator over scalar RV sub-circuits.
 */
#include "Expectation.h"

#include "AnalyticEvaluator.h"
#include "BooleanCircuit.h"
#include "Circuit.h"
#include "CircuitFromMMap.h"
#include "MonteCarloSampler.h"
#include "RandomVariable.h"
#include "RangeCheck.h"
#include "provsql_utils_cpp.h"
#include "semiring/BoolExpr.h"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_moment);
}

#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace provsql {

double evaluateBooleanProbability(const GenericCircuit &gc, gate_t boolRoot)
{
  BooleanCircuit c;
  std::unordered_map<gate_t, gate_t> gc_to_bc;
  for (gate_t u : gc.getInputs()) {
    gc_to_bc[u] = c.setGate(gc.getUUID(u), BooleanGate::IN, gc.getProb(u));
  }
  for (size_t i = 0; i < gc.getNbGates(); ++i) {
    auto u = static_cast<gate_t>(i);
    if (gc.getGateType(u) == gate_mulinput) {
      gc_to_bc[u] = c.setGate(gc.getUUID(u), BooleanGate::MULIN, gc.getProb(u));
      c.setInfo(gc_to_bc[u], gc.getInfos(u).first);
      c.addWire(gc_to_bc[u], gc_to_bc[gc.getWires(u)[0]]);
    }
  }
  semiring::BoolExpr semiring(c);
  gate_t bcRoot = gc.evaluate(boolRoot, gc_to_bc, semiring);

  try {
    return c.independentEvaluation(bcRoot);
  } catch (CircuitException &) {
    if (provsql_rv_mc_samples <= 0) {
      throw CircuitException(
        "evaluateBooleanProbability: subcircuit could not be evaluated "
        "independently and provsql.rv_mc_samples = 0 disables the "
        "Monte Carlo fallback");
    }
    c.rewriteMultivaluedGates();
    return c.monteCarlo(bcRoot,
                        static_cast<unsigned>(provsql_rv_mc_samples));
  }
}

namespace {

using RvSet = std::set<gate_t>;

/// Mixing weight π = P(p = true) for a mixture's Bernoulli wire.
/// For a bare @c gate_input, the probability is the leaf's pinned
/// @c set_prob; for any compound Boolean gate, defer to
/// @c evaluateBooleanProbability.
double mixturePi(const GenericCircuit &gc, gate_t p)
{
  return (gc.getGateType(p) == gate_input)
    ? gc.getProb(p)
    : evaluateBooleanProbability(gc, p);
}

/// Cache of the base-@c gate_rv UUID footprints reachable below each
/// scalar gate, used as the structural-independence witness.  Two
/// children of an arithmetic gate are independent iff their footprints
/// are disjoint -- and therefore the variance and TIMES expectation
/// shortcuts apply.
class FootprintCache {
public:
  explicit FootprintCache(const GenericCircuit &gc) : gc_(gc) {}

  const RvSet &of(gate_t g) {
    auto it = cache_.find(g);
    if (it != cache_.end()) return it->second;
    RvSet s;
    auto type = gc_.getGateType(g);
    if (type == gate_rv) {
      s.insert(g);
    } else if (type == gate_value) {
      // empty -- no RV reached
    } else if (type == gate_arith) {
      for (gate_t c : gc_.getWires(g)) {
        const auto &cs = of(c);
        s.insert(cs.begin(), cs.end());
      }
    } else if (type == gate_mixture) {
      const auto &wires = gc_.getWires(g);
      if (gc_.isCategoricalMixture(g)) {
        // Categorical-form mixture.  Footprint = union of the
        // mulinputs' footprints (each contributes {self, key} via the
        // mulinput branch below), so two categoricals sharing a key
        // overlap on it and are correctly flagged dependent.
        for (std::size_t i = 1; i < wires.size(); ++i) {
          const auto &fm = of(wires[i]);
          s.insert(fm.begin(), fm.end());
        }
      } else if (wires.size() == 3) {
        // Classic 3-wire mixture.  Footprint = footprint(p) ∪
        // footprint(x) ∪ footprint(y).  The Boolean wire is included
        // as a discrete random source so two mixtures whose p's share
        // an atom are correctly recognised as dependent (their branch
        // selection is correlated), bypassing the closed-form
        // independence shortcut.  Recursing into wires[0] (rather than
        // inserting its gate_t directly) generalises that recognition
        // from bare-input Bernoullis to compound Boolean wires.
        const auto &fp = of(wires[0]);
        s.insert(fp.begin(), fp.end());
        const auto &fx = of(wires[1]);
        s.insert(fx.begin(), fx.end());
        const auto &fy = of(wires[2]);
        s.insert(fy.begin(), fy.end());
      }
    } else if (type == gate_mulinput) {
      // A mulinput is a state-carrying atom (so its own gate_t is in
      // its footprint -- two mulinputs of the same group are distinct
      // atoms even though they share a key) *and* references a shared
      // key gate at wires[0] (whose footprint is added so the shared
      // key makes the two mulinputs overlap on it, flagging them as
      // dependent for pairwise_disjoint).
      s.insert(g);
      const auto &wires = gc_.getWires(g);
      if (!wires.empty()) {
        const auto &fk = of(wires[0]);
        s.insert(fk.begin(), fk.end());
      }
    } else if (type == gate_input) {
      // Atomic Boolean leaf.  Use the gate's own UUID as its footprint
      // so two Boolean expressions sharing this input collide on it.
      s.insert(g);
    } else if (type == gate_plus || type == gate_times || type == gate_monus
            || type == gate_project || type == gate_eq
            || type == gate_cmp || type == gate_update) {
      // Boolean gates: footprint is the union of children's footprints.
      // Lets a compound `p` wire feeding a mixture propagate its atom
      // dependencies to FootprintCache for the disjoint-children
      // shortcuts in rec_expectation / rec_variance / rec_raw_moment.
      for (gate_t c : gc_.getWires(g)) {
        const auto &cs = of(c);
        s.insert(cs.begin(), cs.end());
      }
    } else if (type == gate_zero || type == gate_one) {
      // Empty footprint -- a constant-true / constant-false Boolean
      // contributes no shared atoms.
    } else {
      // Unknown scalar gate type: return an empty footprint.  Callers
      // will trip the analytical-decomposition switch and route the
      // gate to the MC fallback (or raise if the fallback is disabled),
      // which is the right behaviour for any unanticipated leaf shape.
    }
    return cache_.emplace(g, std::move(s)).first->second;
  }

private:
  const GenericCircuit &gc_;
  std::unordered_map<gate_t, RvSet> cache_;
};

bool pairwise_disjoint(FootprintCache &fp, const std::vector<gate_t> &children)
{
  RvSet seen;
  for (gate_t c : children) {
    const auto &fpc = fp.of(c);
    for (gate_t r : fpc) {
      if (!seen.insert(r).second) return false;
    }
  }
  return true;
}

unsigned mc_samples_or_throw(const std::string &what)
{
  const int n = provsql_rv_mc_samples;
  if (n <= 0) {
    throw CircuitException(
      what + " could not be decomposed analytically and "
      "provsql.rv_mc_samples = 0 disables the Monte Carlo fallback");
  }
  return static_cast<unsigned>(n);
}

double mc_raw_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                     const std::string &what)
{
  auto samples = monteCarloScalarSamples(gc, g, mc_samples_or_throw(what));
  if (samples.empty()) return 0.0;
  double total = 0.0;
  for (double x : samples) total += std::pow(x, static_cast<double>(k));
  return total / static_cast<double>(samples.size());
}

double mc_central_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                         double mu, const std::string &what)
{
  auto samples = monteCarloScalarSamples(gc, g, mc_samples_or_throw(what));
  if (samples.empty()) return 0.0;
  double total = 0.0;
  for (double x : samples) {
    const double d = x - mu;
    total += std::pow(d, static_cast<double>(k));
  }
  return total / static_cast<double>(samples.size());
}

/// Minimum accepted-sample count for conditional MC moments.  Below
/// this floor we'd be reporting a moment from a handful of accepted
/// draws and the variance of the estimator would be enormous; raise
/// rather than silently return a noisy number.
unsigned min_accepted_floor(unsigned attempted)
{
  unsigned floor = attempted / 1000;
  return floor < 5 ? 5 : floor;
}

void check_acceptance_or_throw(const ConditionalScalarSamples &cs,
                               const std::string &what)
{
  const unsigned floor = min_accepted_floor(cs.attempted);
  if (cs.accepted.size() < floor) {
    throw CircuitException(
      what + ": conditional MC accepted " +
      std::to_string(cs.accepted.size()) + " out of " +
      std::to_string(cs.attempted) +
      " samples (need >= " + std::to_string(floor) +
      "); raise provsql.rv_mc_samples or check that the event is satisfiable");
  }
}

double mc_conditional_raw_moment(const GenericCircuit &gc, gate_t g,
                                 unsigned k, gate_t event_root,
                                 const std::string &what)
{
  auto cs = monteCarloConditionalScalarSamples(
              gc, g, event_root, mc_samples_or_throw(what));
  check_acceptance_or_throw(cs, what);
  double total = 0.0;
  for (double x : cs.accepted) total += std::pow(x, static_cast<double>(k));
  return total / static_cast<double>(cs.accepted.size());
}

double mc_conditional_central_moment(const GenericCircuit &gc, gate_t g,
                                     unsigned k, double mu,
                                     gate_t event_root,
                                     const std::string &what)
{
  auto cs = monteCarloConditionalScalarSamples(
              gc, g, event_root, mc_samples_or_throw(what));
  check_acceptance_or_throw(cs, what);
  double total = 0.0;
  for (double x : cs.accepted) {
    const double d = x - mu;
    total += std::pow(d, static_cast<double>(k));
  }
  return total / static_cast<double>(cs.accepted.size());
}

double binomial(unsigned n, unsigned k)
{
  if (k > n) return 0.0;
  if (k > n - k) k = n - k;
  double r = 1.0;
  for (unsigned i = 1; i <= k; ++i) {
    r *= static_cast<double>(n - i + 1);
    r /= static_cast<double>(i);
  }
  return r;
}

double rec_expectation(const GenericCircuit &gc, gate_t g, FootprintCache &fp);
double rec_variance(const GenericCircuit &gc, gate_t g, FootprintCache &fp);
double rec_raw_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                      FootprintCache &fp);

/// Standard normal pdf φ(z) = exp(-z²/2)/√(2π).
double phi(double z)
{
  static const double INV_SQRT_2PI = 1.0 / std::sqrt(2.0 * M_PI);
  return INV_SQRT_2PI * std::exp(-0.5 * z * z);
}

/// Standard normal CDF Φ(z) = ½(1 + erf(z/√2)).  Mirrors the
/// AnalyticEvaluator::cdfAt Normal branch so the truncation formulas
/// here use the same numerical convention.
double Phi(double z)
{
  static const double SQRT2 = std::sqrt(2.0);
  return 0.5 * (1.0 + std::erf(z / SQRT2));
}

/**
 * @brief Raw moments of @c X ~ Normal(μ, σ) truncated to @c [a, b].
 *
 * Closed form via the integration-by-parts recurrence on the
 * standardised variable Z = (X - μ)/σ:
 *   E[Z^{k}|α<Z<β] = (k-1) E[Z^{k-2}|α<Z<β]
 *                  + (α^{k-1}φ(α) − β^{k-1}φ(β)) / (Φ(β) − Φ(α))
 * with E[Z^0|…] = 1 and E[Z^1|…] = (φ(α) − φ(β)) / (Φ(β) − Φ(α))
 * (Greene, "Econometric Analysis", 5e, App. F).  Then expand
 * E[X^k] = E[(μ + σZ)^k] binomially.
 *
 * @c α = -∞ corresponds to @p a = -INFINITY (semi-infinite left tail);
 * @c β = +∞ to @p b = +INFINITY.  Returns @c NaN if @c P(α<Z<β) is
 * below a numerical floor (so the caller falls through to MC).
 */
double truncated_normal_raw_moment(double mu, double sigma, double a, double b,
                                   unsigned k)
{
  const double alpha = std::isfinite(a) ? (a - mu) / sigma
                                        : -std::numeric_limits<double>::infinity();
  const double beta  = std::isfinite(b) ? (b - mu) / sigma
                                        : +std::numeric_limits<double>::infinity();
  const double Phi_alpha = std::isfinite(alpha) ? Phi(alpha) : 0.0;
  const double Phi_beta  = std::isfinite(beta)  ? Phi(beta)  : 1.0;
  const double Z = Phi_beta - Phi_alpha;
  if (Z < 1e-12) return std::numeric_limits<double>::quiet_NaN();

  const double phi_alpha = std::isfinite(alpha) ? phi(alpha) : 0.0;
  const double phi_beta  = std::isfinite(beta)  ? phi(beta)  : 0.0;

  /* E[Z^k | α<Z<β] via recurrence; store all moments up to k. */
  std::vector<double> M(k + 1, 0.0);
  M[0] = 1.0;
  if (k >= 1) M[1] = (phi_alpha - phi_beta) / Z;
  for (unsigned m = 2; m <= k; ++m) {
    /* α^{m-1}·φ(α) and β^{m-1}·φ(β); take 0 when the endpoint is
     * infinite (the φ factor vanishes faster than any polynomial). */
    double end_term = 0.0;
    if (std::isfinite(alpha))
      end_term += std::pow(alpha, static_cast<double>(m - 1)) * phi_alpha;
    if (std::isfinite(beta))
      end_term -= std::pow(beta, static_cast<double>(m - 1)) * phi_beta;
    M[m] = (m - 1) * M[m - 2] + end_term / Z;
  }

  /* E[X^k] = E[(μ + σZ)^k] = Σ_{i=0..k} C(k,i) μ^{k-i} σ^i E[Z^i|…]. */
  double total = 0.0;
  for (unsigned i = 0; i <= k; ++i) {
    total += binomial(k, i)
           * std::pow(mu, static_cast<double>(k - i))
           * std::pow(sigma, static_cast<double>(i))
           * M[i];
  }
  return total;
}

/**
 * @brief Raw moments of @c X ~ Uniform(p1, p2) truncated to @c [a, b].
 *
 * The intersection @c [a', b'] = [max(p1,a), min(p2,b)] is uniform;
 * its k-th raw moment is @c (b'^{k+1} - a'^{k+1}) / ((k+1)(b' - a')).
 */
double truncated_uniform_raw_moment(double p1, double p2, double a, double b,
                                    unsigned k)
{
  const double lo = std::max(p1, a);
  const double hi = std::min(p2, b);
  if (hi <= lo) return std::numeric_limits<double>::quiet_NaN();
  if (k == 0) return 1.0;
  return (std::pow(hi, static_cast<double>(k + 1))
        - std::pow(lo, static_cast<double>(k + 1)))
       / ((k + 1) * (hi - lo));
}

/**
 * @brief Raw moments of @c X ~ Exp(λ) truncated to @c [a, b].
 *
 * Decomposes via change of variable Y = X - max(a,0):
 *   - left endpoint @c a > 0, right endpoint @c b = +∞: by
 *     memorylessness @c X | X>a is distributed as @c a + Exp(λ), so
 *     @c E[X^k|X>a] = Σ_{i=0..k} C(k,i) a^{k-i} · i!/λ^i.
 *   - finite @c [a, b] (with @c a ≥ 0, @c b < ∞): integrate
 *     @c x^k λ e^{-λx} dx by parts and divide by the truncation mass
 *     @c e^{-λa} - e^{-λb}.  Uses the recurrence
 *     @c I_k = k I_{k-1} / λ - (b^k e^{-λb} - a^k e^{-λa}) / λ
 *     with @c I_0 = e^{-λa} - e^{-λb}.
 */
double truncated_exponential_raw_moment(double lambda, double a, double b,
                                        unsigned k)
{
  const double aa = std::max(a, 0.0);  /* Exp support is [0, +∞) */
  if (std::isfinite(b)) {
    if (b <= aa) return std::numeric_limits<double>::quiet_NaN();
    /* Finite-interval recurrence on I_k = ∫_{aa}^{b} x^k λ e^{-λx} dx. */
    const double e_a = std::exp(-lambda * aa);
    const double e_b = std::exp(-lambda * b);
    const double Z = e_a - e_b;  /* P(aa < X < b) */
    if (Z < 1e-12) return std::numeric_limits<double>::quiet_NaN();
    if (k == 0) return 1.0;
    /* Integration by parts: ∫ x^k λ e^{-λx} dx = -x^k e^{-λx} + k ∫ x^{k-1} e^{-λx} dx
     * so I_k (with λ factor folded into the e^{-λx}·λ dx term) follows:
     *   I_k = [aa^k e^{-λaa} - b^k e^{-λb}] + (k/λ) · I_{k-1}_no_lambda
     * where I_{k-1}_no_lambda = ∫ x^{k-1} e^{-λx} dx = I_{k-1}/λ.
     * Cleaner: compute J_k = ∫_{aa}^{b} x^k e^{-λx} dx via
     *   J_0 = Z/λ; J_k = (aa^k e^{-λaa} - b^k e^{-λb})/λ + (k/λ) J_{k-1}.
     * Then E[X^k|aa<X<b] = λ J_k / Z. */
    std::vector<double> J(k + 1, 0.0);
    J[0] = Z / lambda;
    for (unsigned m = 1; m <= k; ++m) {
      const double endpoint = std::pow(aa, static_cast<double>(m)) * e_a
                            - std::pow(b,  static_cast<double>(m)) * e_b;
      J[m] = endpoint / lambda + (m / lambda) * J[m - 1];
    }
    return lambda * J[k] / Z;
  }
  /* Semi-infinite right tail [aa, +∞): memorylessness. */
  double total = 0.0;
  double fact_i = 1.0;
  for (unsigned i = 0; i <= k; ++i) {
    total += binomial(k, i)
           * std::pow(aa, static_cast<double>(k - i))
           * fact_i / std::pow(lambda, static_cast<double>(i));
    fact_i *= (i + 1);
  }
  return total;
}

/**
 * @brief Try to evaluate @f$E[X^k \mid A]@f$ in closed form.
 *
 * Fires only when @p root is a bare @c gate_rv of a recognised kind
 * (Normal / Uniform / Exponential) and the event walk under
 * @p event_root collects a sound interval constraint on it.
 * Otherwise returns @c std::nullopt and the caller falls through to
 * MC rejection.
 *
 * For @p central, returns @f$E[(X - \mu_A)^k \mid A]@f$ where
 * @f$\mu_A@f$ is the closed-form conditional mean obtained by
 * recursing on @c k = 1, then binomially expanding the central
 * moment in terms of the raw moments.
 */
std::optional<double>
try_truncated_closed_form(const GenericCircuit &gc, gate_t root,
                          gate_t event_root, unsigned k, bool central)
{
  auto m = matchTruncatedSingleRv(gc, root, event_root);
  if (!m) return std::nullopt;
  const DistributionSpec &spec = m->spec;
  const double lo = m->lo, hi = m->hi;

  /* Closed-form raw moment of the truncated distribution. */
  auto raw = [&](unsigned q) -> std::optional<double> {
    if (q == 0) return 1.0;
    double r = std::numeric_limits<double>::quiet_NaN();
    switch (spec.kind) {
      case DistKind::Normal:
        r = truncated_normal_raw_moment(spec.p1, spec.p2, lo, hi, q);
        break;
      case DistKind::Uniform:
        r = truncated_uniform_raw_moment(spec.p1, spec.p2, lo, hi, q);
        break;
      case DistKind::Exponential:
        r = truncated_exponential_raw_moment(spec.p1, lo, hi, q);
        break;
      case DistKind::Erlang:
        /* Truncated Erlang moments require the regularised lower
         * incomplete gamma; out of scope for v1.  Fall through to MC. */
        return std::nullopt;
    }
    if (std::isnan(r)) return std::nullopt;
    return r;
  };

  if (!central) return raw(k);

  /* Central: E[(X - μ_A)^k | A] = Σ_{i=0..k} C(k,i) (-μ_A)^{k-i} E[X^i | A]. */
  auto mu_opt = raw(1);
  if (!mu_opt) return std::nullopt;
  const double mu = *mu_opt;
  if (k == 1) return 0.0;
  double total = 0.0;
  for (unsigned i = 0; i <= k; ++i) {
    auto m_i = raw(i);
    if (!m_i) return std::nullopt;
    total += binomial(k, i)
           * std::pow(-mu, static_cast<double>(k - i)) * (*m_i);
  }
  return total;
}

double rec_expectation(const GenericCircuit &gc, gate_t g, FootprintCache &fp)
{
  const auto type = gc.getGateType(g);
  switch (type) {
    case gate_value:
      return parseDoubleStrict(gc.getExtra(g));
    case gate_rv: {
      auto spec = parse_distribution_spec(gc.getExtra(g));
      if (!spec)
        throw CircuitException(
          "Expectation: malformed gate_rv extra: " + gc.getExtra(g));
      return analytical_mean(*spec);
    }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &wires = gc.getWires(g);
      switch (op) {
        case PROVSQL_ARITH_PLUS: {
          double s = 0.0;
          for (gate_t c : wires) s += rec_expectation(gc, c, fp);
          return s;
        }
        case PROVSQL_ARITH_MINUS: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith MINUS must be binary");
          return rec_expectation(gc, wires[0], fp)
               - rec_expectation(gc, wires[1], fp);
        }
        case PROVSQL_ARITH_NEG: {
          if (wires.size() != 1)
            throw CircuitException("gate_arith NEG must be unary");
          return -rec_expectation(gc, wires[0], fp);
        }
        case PROVSQL_ARITH_TIMES: {
          if (pairwise_disjoint(fp, wires)) {
            double p = 1.0;
            for (gate_t c : wires) p *= rec_expectation(gc, c, fp);
            return p;
          }
          return mc_raw_moment(gc, g, 1,
            "Expectation of gate_arith TIMES with shared random variables");
        }
        case PROVSQL_ARITH_DIV: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith DIV must be binary");
          if (gc.getGateType(wires[1]) == gate_value) {
            const double divisor = parseDoubleStrict(gc.getExtra(wires[1]));
            return rec_expectation(gc, wires[0], fp) / divisor;
          }
          return mc_raw_moment(gc, g, 1,
            "Expectation of gate_arith DIV with non-constant divisor");
        }
      }
      throw CircuitException(
        "Expectation: unknown gate_arith op tag: " +
        std::to_string(static_cast<unsigned>(op)));
    }
    case gate_mixture: {
      const auto &wires = gc.getWires(g);
      if (gc.isCategoricalMixture(g)) {
        // Categorical mixture: E[M] = Σ π_i · v_i, where each mulinput
        // mul_i carries π_i in set_prob and v_i in extra.
        double s = 0.0;
        for (std::size_t i = 1; i < wires.size(); ++i) {
          s += gc.getProb(wires[i])
             * parseDoubleStrict(gc.getExtra(wires[i]));
        }
        return s;
      }
      // E[mixture(p, X, Y)] = π·E[X] + (1-π)·E[Y], where π = P(p = true).
      // For a bare gate_input p, π is the leaf's pinned set_prob.  For
      // a compound Boolean p, route through evaluateBooleanProbability
      // so π honors the tuple-independent semantics of the Boolean DAG.
      if (wires.size() != 3)
        throw CircuitException(
          "Expectation: gate_mixture must have exactly three children");
      const double pi = mixturePi(gc, wires[0]);
      return pi        * rec_expectation(gc, wires[1], fp)
           + (1.0 - pi) * rec_expectation(gc, wires[2], fp);
    }
    default:
      return mc_raw_moment(gc, g, 1,
        "Expectation of gate type " + std::string(gate_type_name[type]));
  }
}

double rec_variance(const GenericCircuit &gc, gate_t g, FootprintCache &fp)
{
  const auto type = gc.getGateType(g);
  switch (type) {
    case gate_value:
      return 0.0;
    case gate_rv: {
      auto spec = parse_distribution_spec(gc.getExtra(g));
      if (!spec)
        throw CircuitException(
          "Variance: malformed gate_rv extra: " + gc.getExtra(g));
      return analytical_variance(*spec);
    }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &wires = gc.getWires(g);
      auto mc_var = [&](const std::string &what) {
        const double mu = mc_raw_moment(gc, g, 1, what);
        return mc_central_moment(gc, g, 2, mu, what);
      };
      switch (op) {
        case PROVSQL_ARITH_PLUS: {
          if (pairwise_disjoint(fp, wires)) {
            double s = 0.0;
            for (gate_t c : wires) s += rec_variance(gc, c, fp);
            return s;
          }
          return mc_var(
            "Variance of gate_arith PLUS with shared random variables");
        }
        case PROVSQL_ARITH_MINUS: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith MINUS must be binary");
          if (pairwise_disjoint(fp, wires)) {
            return rec_variance(gc, wires[0], fp)
                 + rec_variance(gc, wires[1], fp);
          }
          return mc_var(
            "Variance of gate_arith MINUS with shared random variables");
        }
        case PROVSQL_ARITH_NEG: {
          if (wires.size() != 1)
            throw CircuitException("gate_arith NEG must be unary");
          return rec_variance(gc, wires[0], fp);
        }
        case PROVSQL_ARITH_TIMES: {
          if (pairwise_disjoint(fp, wires)) {
            // Var(prod Xi) = prod E[Xi^2] - (prod E[Xi])^2
            //              = prod (Var[Xi] + E[Xi]^2) - (prod E[Xi])^2
            double prod_e2 = 1.0;
            double prod_e1 = 1.0;
            for (gate_t c : wires) {
              const double mu_c = rec_expectation(gc, c, fp);
              const double v_c  = rec_variance(gc, c, fp);
              prod_e2 *= (v_c + mu_c * mu_c);
              prod_e1 *= mu_c;
            }
            return prod_e2 - prod_e1 * prod_e1;
          }
          return mc_var(
            "Variance of gate_arith TIMES with shared random variables");
        }
        case PROVSQL_ARITH_DIV: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith DIV must be binary");
          if (gc.getGateType(wires[1]) == gate_value) {
            const double divisor = parseDoubleStrict(gc.getExtra(wires[1]));
            return rec_variance(gc, wires[0], fp) / (divisor * divisor);
          }
          return mc_var(
            "Variance of gate_arith DIV with non-constant divisor");
        }
      }
      throw CircuitException(
        "Variance: unknown gate_arith op tag: " +
        std::to_string(static_cast<unsigned>(op)));
    }
    case gate_mixture: {
      const auto &wires = gc.getWires(g);
      if (gc.isCategoricalMixture(g)) {
        // Categorical mixture: Var(M) = Σ π_i v_i² − (Σ π_i v_i)².
        double e1 = 0.0, e2 = 0.0;
        for (std::size_t i = 1; i < wires.size(); ++i) {
          const double p = gc.getProb(wires[i]);
          const double v = parseDoubleStrict(gc.getExtra(wires[i]));
          e1 += p * v;
          e2 += p * v * v;
        }
        return e2 - e1 * e1;
      }
      // Var(M) = π·(Var(X) + E[X]²) + (1-π)·(Var(Y) + E[Y]²) - E[M]²
      // (law of total variance specialised to a Bernoulli mixture).
      if (wires.size() != 3)
        throw CircuitException(
          "Variance: gate_mixture must have exactly three children");
      const double pi = mixturePi(gc, wires[0]);
      const double ex = rec_expectation(gc, wires[1], fp);
      const double ey = rec_expectation(gc, wires[2], fp);
      const double vx = rec_variance(gc, wires[1], fp);
      const double vy = rec_variance(gc, wires[2], fp);
      const double em = pi * ex + (1.0 - pi) * ey;
      return pi        * (vx + ex * ex)
           + (1.0 - pi) * (vy + ey * ey)
           - em * em;
    }
    default: {
      const std::string what =
        "Variance of gate type " + std::string(gate_type_name[type]);
      const double mu = mc_raw_moment(gc, g, 1, what);
      return mc_central_moment(gc, g, 2, mu, what);
    }
  }
}

double rec_raw_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                      FootprintCache &fp)
{
  if (k == 0) return 1.0;
  if (k == 1) return rec_expectation(gc, g, fp);

  const auto type = gc.getGateType(g);
  switch (type) {
    case gate_value:
      return std::pow(parseDoubleStrict(gc.getExtra(g)),
                      static_cast<double>(k));
    case gate_rv: {
      auto spec = parse_distribution_spec(gc.getExtra(g));
      if (!spec)
        throw CircuitException(
          "Moment: malformed gate_rv extra: " + gc.getExtra(g));
      return analytical_raw_moment(*spec, k);
    }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &wires = gc.getWires(g);
      switch (op) {
        case PROVSQL_ARITH_NEG: {
          if (wires.size() != 1)
            throw CircuitException("gate_arith NEG must be unary");
          const double v = rec_raw_moment(gc, wires[0], k, fp);
          return ((k % 2 == 0) ? 1.0 : -1.0) * v;
        }
        case PROVSQL_ARITH_PLUS: {
          if (pairwise_disjoint(fp, wires)) {
            // Fold-left: m_acc[i] holds E[(X1 + ... + Xj)^i] for the
            // first j children processed; combining with the next
            // independent child Y uses the binomial theorem.
            std::vector<double> m_acc(k + 1, 0.0);
            for (unsigned i = 0; i <= k; ++i)
              m_acc[i] = rec_raw_moment(gc, wires[0], i, fp);
            for (size_t w = 1; w < wires.size(); ++w) {
              std::vector<double> next(k + 1, 0.0);
              std::vector<double> moments_y(k + 1, 0.0);
              for (unsigned i = 0; i <= k; ++i)
                moments_y[i] = rec_raw_moment(gc, wires[w], i, fp);
              for (unsigned kp = 0; kp <= k; ++kp) {
                double total = 0.0;
                for (unsigned i = 0; i <= kp; ++i) {
                  total += binomial(kp, i) * m_acc[i] * moments_y[kp - i];
                }
                next[kp] = total;
              }
              m_acc = std::move(next);
            }
            return m_acc[k];
          }
          return mc_raw_moment(gc, g, k,
            "Raw moment of gate_arith PLUS with shared random variables");
        }
        case PROVSQL_ARITH_MINUS: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith MINUS must be binary");
          if (pairwise_disjoint(fp, wires)) {
            double total = 0.0;
            for (unsigned i = 0; i <= k; ++i) {
              const double sign = ((k - i) % 2 == 0) ? 1.0 : -1.0;
              total += binomial(k, i)
                     * rec_raw_moment(gc, wires[0], i, fp)
                     * sign
                     * rec_raw_moment(gc, wires[1], k - i, fp);
            }
            return total;
          }
          return mc_raw_moment(gc, g, k,
            "Raw moment of gate_arith MINUS with shared random variables");
        }
        case PROVSQL_ARITH_TIMES: {
          if (pairwise_disjoint(fp, wires)) {
            // (prod Xi)^k = prod Xi^k; under independence E factors.
            double p = 1.0;
            for (gate_t c : wires) p *= rec_raw_moment(gc, c, k, fp);
            return p;
          }
          return mc_raw_moment(gc, g, k,
            "Raw moment of gate_arith TIMES with shared random variables");
        }
        case PROVSQL_ARITH_DIV: {
          if (wires.size() != 2)
            throw CircuitException("gate_arith DIV must be binary");
          if (gc.getGateType(wires[1]) == gate_value) {
            const double divisor = parseDoubleStrict(gc.getExtra(wires[1]));
            return rec_raw_moment(gc, wires[0], k, fp)
                 / std::pow(divisor, static_cast<double>(k));
          }
          return mc_raw_moment(gc, g, k,
            "Raw moment of gate_arith DIV with non-constant divisor");
        }
      }
      throw CircuitException(
        "Moment: unknown gate_arith op tag: " +
        std::to_string(static_cast<unsigned>(op)));
    }
    case gate_mixture: {
      const auto &wires = gc.getWires(g);
      if (gc.isCategoricalMixture(g)) {
        // Categorical mixture: E[M^k] = Σ π_i v_i^k.
        double s = 0.0;
        for (std::size_t i = 1; i < wires.size(); ++i) {
          const double v = parseDoubleStrict(gc.getExtra(wires[i]));
          s += gc.getProb(wires[i])
             * std::pow(v, static_cast<double>(k));
        }
        return s;
      }
      // E[M^k] = π·E[X^k] + (1-π)·E[Y^k].
      if (wires.size() != 3)
        throw CircuitException(
          "Moment: gate_mixture must have exactly three children");
      const double pi = mixturePi(gc, wires[0]);
      return pi        * rec_raw_moment(gc, wires[1], k, fp)
           + (1.0 - pi) * rec_raw_moment(gc, wires[2], k, fp);
    }
    default:
      return mc_raw_moment(gc, g, k,
        "Raw moment of gate type " + std::string(gate_type_name[type]));
  }
}

}  // namespace

/* Conditional dispatch helpers: try closed-form first, fall through
 * to MC rejection.  Used by all four public compute_* entries to keep
 * the conditional logic in one place and the unconditional path
 * unchanged. */
namespace {

double conditional_raw_moment(const GenericCircuit &gc, gate_t root,
                              unsigned k, gate_t event_root)
{
  if (k == 0) return 1.0;
  if (auto cf = try_truncated_closed_form(gc, root, event_root, k, false))
    return *cf;
  return mc_conditional_raw_moment(
    gc, root, k, event_root,
    "Conditional raw moment of gate type " +
      std::string(gate_type_name[gc.getGateType(root)]));
}

double conditional_central_moment(const GenericCircuit &gc, gate_t root,
                                  unsigned k, gate_t event_root)
{
  if (k == 0) return 1.0;
  if (k == 1) return 0.0;
  if (auto cf = try_truncated_closed_form(gc, root, event_root, k, true))
    return *cf;
  /* MC central: need μ_A first. */
  const double mu = conditional_raw_moment(gc, root, 1, event_root);
  return mc_conditional_central_moment(
    gc, root, k, mu, event_root,
    "Conditional central moment of gate type " +
      std::string(gate_type_name[gc.getGateType(root)]));
}

}  // namespace

double compute_expectation(const GenericCircuit &gc, gate_t root,
                           std::optional<gate_t> event_root)
{
  if (event_root.has_value())
    return conditional_raw_moment(gc, root, 1, *event_root);
  FootprintCache fp(gc);
  return rec_expectation(gc, root, fp);
}

double compute_variance(const GenericCircuit &gc, gate_t root,
                        std::optional<gate_t> event_root)
{
  if (event_root.has_value())
    return conditional_central_moment(gc, root, 2, *event_root);
  FootprintCache fp(gc);
  return rec_variance(gc, root, fp);
}

double compute_raw_moment(const GenericCircuit &gc, gate_t root, unsigned k,
                          std::optional<gate_t> event_root)
{
  if (event_root.has_value())
    return conditional_raw_moment(gc, root, k, *event_root);
  FootprintCache fp(gc);
  return rec_raw_moment(gc, root, k, fp);
}

double compute_central_moment(const GenericCircuit &gc, gate_t root, unsigned k,
                              std::optional<gate_t> event_root)
{
  if (event_root.has_value())
    return conditional_central_moment(gc, root, k, *event_root);
  if (k == 0) return 1.0;
  if (k == 1) return 0.0;
  FootprintCache fp(gc);
  if (k == 2) return rec_variance(gc, root, fp);
  // E[(X - mu)^k] = sum_{i=0}^{k} C(k, i) (-mu)^(k-i) E[X^i]
  const double mu = rec_expectation(gc, root, fp);
  double total = 0.0;
  for (unsigned i = 0; i <= k; ++i) {
    const double mu_pow = std::pow(-mu, static_cast<double>(k - i));
    total += binomial(k, i) * mu_pow * rec_raw_moment(gc, root, i, fp);
  }
  return total;
}

}  // namespace provsql

extern "C" {

/**
 * @brief SQL: rv_moment(token uuid, k integer, central boolean,
 *                       prov uuid DEFAULT gate_one()) -> float8
 *
 * Single C entry point shared by the @c expected / @c variance /
 * @c moment / @c central_moment SQL functions.  The SQL wrappers
 * select the (k, central) pair that matches their semantics:
 * - @c expected(rv, prov): k=1, central=false.
 * - @c variance(rv, prov): k=2, central=true.
 * - @c moment(rv, k, prov): central=false.
 * - @c central_moment(rv, k, prov): central=true.
 *
 * The @p prov argument carries the conditioning event: typically the
 * row's @c provenance() gate after a @c WHERE predicate folded a
 * @c gate_cmp into it.  When @p prov resolves to @c gate_one (the
 * default, or the load-time simplification of any always-true
 * sub-circuit) the unconditional path runs unchanged.  Otherwise we
 * load a JOINT circuit reaching both roots, so shared @c gate_rv
 * leaves collapse to a single @c gate_t -- the property the
 * conditional MC sampler relies on to couple the indicator's draw
 * with the value's draw.
 */
Datum rv_moment(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    const int32 k_signed = PG_GETARG_INT32(1);
    const bool central = PG_GETARG_BOOL(2);
    pg_uuid_t *prov = PG_GETARG_UUID_P(3);

    if (k_signed < 0)
      provsql_error("rv_moment: k must be non-negative (got %d)", k_signed);
    const unsigned k = static_cast<unsigned>(k_signed);

    gate_t root_gate, event_gate;
    auto gc = getJointCircuit(*token, *prov, root_gate, event_gate);

    /* gate_one event = unconditional after load-time simplification. */
    std::optional<gate_t> event_opt;
    if (gc.getGateType(event_gate) != gate_one)
      event_opt = event_gate;

    double result;
    if (central)
      result = provsql::compute_central_moment(gc, root_gate, k, event_opt);
    else if (k == 1)
      result = provsql::compute_expectation(gc, root_gate, event_opt);
    else
      result = provsql::compute_raw_moment(gc, root_gate, k, event_opt);
    return Float8GetDatum(result);
  } catch (const std::exception &e) {
    provsql_error("rv_moment: %s", e.what());
  } catch (...) {
    provsql_error("rv_moment: unknown exception");
  }
  PG_RETURN_NULL();
}

}  // extern "C"
