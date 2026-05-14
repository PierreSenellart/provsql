/**
 * @file MonteCarloSampler.cpp
 * @brief Implementation of the RV-aware Monte Carlo sampler.
 */
#include "MonteCarloSampler.h"
#include "Aggregation.h"
#include "RandomVariable.h"
#include "RangeCheck.h"        // collectRvConstraints
#include "Circuit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stack>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace provsql {

namespace {

/// Seed an mt19937_64 from the provsql.monte_carlo_seed GUC.
std::mt19937_64 seedRng()
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

bool applyCmp(double l, ComparisonOperator op, double r)
{
  // IEEE 754 semantics: any comparison involving NaN is false except !=.
  switch(op) {
    case ComparisonOperator::LT: return l < r;
    case ComparisonOperator::LE: return l <= r;
    case ComparisonOperator::EQ: return l == r;
    case ComparisonOperator::NE: return l != r;
    case ComparisonOperator::GE: return l >= r;
    case ComparisonOperator::GT: return l > r;
  }
  return false;
}

/// Per-iteration sampler state shared between the Boolean and scalar
/// recursions.
class Sampler {
public:
  Sampler(const GenericCircuit &gc, std::mt19937_64 &rng)
    : gc_(gc), rng_(rng) {}

  /// Reset per-iteration memo caches.
  void resetIteration() {
    bool_cache_.clear();
    scalar_cache_.clear();
  }

  bool evalBool(gate_t g);
  double evalScalar(gate_t g);

private:
  const GenericCircuit &gc_;
  std::mt19937_64 &rng_;
  std::unordered_map<gate_t, bool> bool_cache_;
  std::unordered_map<gate_t, double> scalar_cache_;
};

bool Sampler::evalBool(gate_t g)
{
  auto it = bool_cache_.find(g);
  if(it != bool_cache_.end()) return it->second;

  bool result = false;
  const auto type = gc_.getGateType(g);
  const auto &wires = gc_.getWires(g);

  switch(type) {
    case gate_input:
    case gate_update:
    {
      std::uniform_real_distribution<double> u(0.0, 1.0);
      result = u(rng_) < gc_.getProb(g);
      break;
    }
    case gate_plus:
      result = false;
      for(gate_t c : wires) {
        if(evalBool(c)) { result = true; break; }
      }
      break;
    case gate_times:
      result = true;
      for(gate_t c : wires) {
        if(!evalBool(c)) { result = false; break; }
      }
      break;
    case gate_monus:
      if(wires.size() != 2)
        throw CircuitException("gate_monus must have exactly two children");
      result = evalBool(wires[0]) && !evalBool(wires[1]);
      break;
    case gate_zero:
      result = false;
      break;
    case gate_one:
      result = true;
      break;
    case gate_cmp:
    {
      if(wires.size() != 2)
        throw CircuitException("gate_cmp must have exactly two children");
      bool ok;
      ComparisonOperator op = cmpOpFromOid(gc_.getInfos(g).first, ok);
      if(!ok)
        throw CircuitException(
                "gate_cmp: unsupported operator OID " +
                std::to_string(gc_.getInfos(g).first));
      double l = evalScalar(wires[0]);
      double r = evalScalar(wires[1]);
      result = applyCmp(l, op, r);
      break;
    }
    case gate_mulinput:
      throw CircuitException(
              "Monte Carlo over circuits containing gate_mulinput "
              "is not yet supported on the RV path");
    case gate_delta:
      // δ-semiring operator: identity on the Boolean semiring, so the
      // sampled truth value is just the wrapped child's.  Showed up
      // when conditioning on a row's provenance() in an aggregate
      // query (HAVING / GROUP BY paths can splice δ over the
      // semimod's k-side).
      if(wires.size() != 1)
        throw CircuitException("gate_delta must have exactly one child");
      result = evalBool(wires[0]);
      break;
    default:
      throw CircuitException(
              "Unsupported gate type in Boolean evaluation: " +
              std::string(gate_type_name[type]));
  }

  bool_cache_[g] = result;
  return result;
}

double Sampler::evalScalar(gate_t g)
{
  auto it = scalar_cache_.find(g);
  if(it != scalar_cache_.end()) return it->second;

  double result = 0.0;
  const auto type = gc_.getGateType(g);
  const auto &wires = gc_.getWires(g);

  switch(type) {
    case gate_value:
      result = parseDoubleStrict(gc_.getExtra(g));
      break;
    case gate_rv:
    {
      auto spec = parse_distribution_spec(gc_.getExtra(g));
      if(!spec)
        throw CircuitException(
                "Malformed gate_rv extra: " + gc_.getExtra(g));
      switch(spec->kind) {
        case DistKind::Normal: {
          std::normal_distribution<double> d(spec->p1, spec->p2);
          result = d(rng_);
          break;
        }
        case DistKind::Uniform: {
          std::uniform_real_distribution<double> d(spec->p1, spec->p2);
          result = d(rng_);
          break;
        }
        case DistKind::Exponential: {
          std::exponential_distribution<double> d(spec->p1);
          result = d(rng_);
          break;
        }
        case DistKind::Erlang: {
          /* Gamma(shape, scale) with integer shape k and scale 1/λ
           * samples Erlang(k, λ) directly.  std::gamma_distribution
           * uses the rate's inverse as its scale parameter. */
          std::gamma_distribution<double> d(spec->p1, 1.0 / spec->p2);
          result = d(rng_);
          break;
        }
      }
      break;
    }
    case gate_arith:
    {
      if(wires.empty())
        throw CircuitException("gate_arith must have at least one child");
      auto op = static_cast<provsql_arith_op>(gc_.getInfos(g).first);
      switch(op) {
        case PROVSQL_ARITH_PLUS:
          result = 0.0;
          for(gate_t c : wires) result += evalScalar(c);
          break;
        case PROVSQL_ARITH_TIMES:
          result = 1.0;
          for(gate_t c : wires) result *= evalScalar(c);
          break;
        case PROVSQL_ARITH_MINUS:
          if(wires.size() != 2)
            throw CircuitException("gate_arith MINUS must be binary");
          result = evalScalar(wires[0]) - evalScalar(wires[1]);
          break;
        case PROVSQL_ARITH_DIV:
          if(wires.size() != 2)
            throw CircuitException("gate_arith DIV must be binary");
          result = evalScalar(wires[0]) / evalScalar(wires[1]);
          break;
        case PROVSQL_ARITH_NEG:
          if(wires.size() != 1)
            throw CircuitException("gate_arith NEG must be unary");
          result = -evalScalar(wires[0]);
          break;
        default:
          throw CircuitException(
                  "Unknown gate_arith operator tag: " +
                  std::to_string(static_cast<unsigned>(op)));
      }
      break;
    }
    case gate_agg:
    {
      // HAVING-style aggregate evaluated per MC iteration: walk the
      // gate_semimod children, keep the rows whose k_gate fires in
      // this world, push their value into a reusable Aggregator,
      // return the finalised scalar.  Closes the priority-4-era gap
      // that made `WHERE rv > 0 GROUP BY x HAVING count(*) > 1`
      // structural-only (see continuous_selection.sql section G).
      //
      // Type plan: we evaluate every numeric path in float8 to stay
      // inside evalScalar's return type.  COUNT is normalised by
      // makeAggregator to SumAgg<long>, so we feed it 1L per kept row
      // without evaluating the gate_one value child; SUM / AVG / MIN /
      // MAX consume the value via evalScalar.  Empty groups finalise
      // to NONE; we surface NaN, which compares false under IEEE on
      // any subsequent gate_cmp -- the SQL convention for HAVING on
      // an empty group.
      AggregationOperator op =
        getAggregationOperator(gc_.getInfos(g).first);
      std::unique_ptr<Aggregator> agg =
        (op == AggregationOperator::COUNT)
          ? makeAggregator(op, ValueType::INT)
          : makeAggregator(op, ValueType::FLOAT);
      if(!agg)
        throw CircuitException(
                "gate_agg: makeAggregator returned null for op " +
                std::to_string(static_cast<int>(op)));
      for(gate_t child : wires) {
        if(gc_.getGateType(child) != gate_semimod) continue;
        const auto &sm = gc_.getWires(child);
        if(sm.size() != 2) continue;
        if(!evalBool(sm[0])) continue;
        if(op == AggregationOperator::COUNT) {
          agg->add(AggValue(1L));
        } else {
          agg->add(AggValue(evalScalar(sm[1])));
        }
      }
      AggValue r = agg->finalize();
      switch(r.getType()) {
        case ValueType::INT:
          result = static_cast<double>(std::get<long>(r.v));
          break;
        case ValueType::FLOAT:
          result = std::get<double>(r.v);
          break;
        case ValueType::NONE:
          result = std::numeric_limits<double>::quiet_NaN();
          break;
        default:
          throw CircuitException(
                  "gate_agg: unsupported aggregate result ValueType in MC");
      }
      break;
    }
    case gate_mixture:
    {
      // Two shapes of gate_mixture share this case:
      //
      //  - Classic 3-wire:  [p_token, x_token, y_token].  Draw the
      //    Bernoulli via evalBool, which handles gate_input by
      //    sampling uniform(0,1) < get_prob and memoises on
      //    bool_cache_; two mixtures sharing the same p_token
      //    therefore see the same draw, and any unrelated Boolean
      //    parent of p_token stays in sync.
      //
      //  - Categorical N-wire: [key, mul_1, ..., mul_n].  Built
      //    directly by the @c provsql.categorical SQL constructor;
      //    each mul_i carries its probability in set_prob and its
      //    outcome value in extra.
      //    We draw a single uniform[0,1) per block, walk the
      //    cumulative probabilities to pick a mulinput, and stash the
      //    Boolean truth values into bool_cache_ so any downstream
      //    Boolean consumer of the mulinputs (independentEvaluation,
      //    OR/AND parents) sees a consistent sampled outcome.
      if(gc_.isCategoricalMixture(g)) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double r = u(rng_);
        double cum = 0.0;
        // Default to the last mulinput in case floating-point cumulative
        // sums leave us shy of 1.0 by a few ULPs.
        std::size_t chosen = wires.size() - 1;
        for(std::size_t i = 1; i < wires.size(); ++i) {
          cum += gc_.getProb(wires[i]);
          if(r < cum) { chosen = i; break; }
        }
        for(std::size_t i = 1; i < wires.size(); ++i) {
          bool_cache_[wires[i]] = (i == chosen);
        }
        result = parseDoubleStrict(gc_.getExtra(wires[chosen]));
        break;
      }
      if(wires.size() != 3)
        throw CircuitException(
                "gate_mixture must have exactly three children "
                "[p_token, x_token, y_token]");
      result = evalBool(wires[0]) ? evalScalar(wires[1])
                                  : evalScalar(wires[2]);
      break;
    }
    default:
      throw CircuitException(
              "Unsupported gate type in scalar evaluation: " +
              std::string(gate_type_name[type]));
  }

  scalar_cache_[g] = result;
  return result;
}

}  // namespace

double monteCarloRV(const GenericCircuit &gc, gate_t root, unsigned samples)
{
  std::mt19937_64 rng = seedRng();
  Sampler sampler(gc, rng);

  unsigned success = 0;
  for(unsigned i = 0; i < samples; ++i) {
    sampler.resetIteration();
    if(sampler.evalBool(root))
      ++success;

    if(provsql_interrupted)
      throw CircuitException(
              "Interrupted after " + std::to_string(i + 1) + " samples");
  }
  return success * 1.0 / samples;
}

std::vector<double> monteCarloJointDistribution(
  const GenericCircuit &gc,
  const std::vector<gate_t> &cmps,
  unsigned samples)
{
  const unsigned k = cmps.size();
  if (k == 0)
    throw CircuitException(
      "monteCarloJointDistribution: empty cmps list");
  if (k > 30)
    throw CircuitException(
      "monteCarloJointDistribution: too many cmps in island ("
      + std::to_string(k) + " > 30)");

  std::mt19937_64 rng = seedRng();
  Sampler sampler(gc, rng);

  const std::size_t nb_outcomes = std::size_t{1} << k;
  std::vector<unsigned> counts(nb_outcomes, 0);

  for (unsigned i = 0; i < samples; ++i) {
    sampler.resetIteration();
    std::size_t w = 0;
    for (unsigned j = 0; j < k; ++j) {
      if (sampler.evalBool(cmps[j])) w |= (std::size_t{1} << j);
    }
    ++counts[w];
    if (provsql_interrupted)
      throw CircuitException(
        "Interrupted after " + std::to_string(i + 1) + " samples");
  }

  std::vector<double> probs(nb_outcomes);
  for (std::size_t w = 0; w < nb_outcomes; ++w)
    probs[w] = counts[w] * 1.0 / samples;
  return probs;
}

std::vector<double> monteCarloScalarSamples(
  const GenericCircuit &gc, gate_t root, unsigned samples)
{
  std::mt19937_64 rng = seedRng();
  Sampler sampler(gc, rng);

  std::vector<double> out;
  out.reserve(samples);
  for(unsigned i = 0; i < samples; ++i) {
    sampler.resetIteration();
    out.push_back(sampler.evalScalar(root));

    if(provsql_interrupted)
      throw CircuitException(
              "Interrupted after " + std::to_string(i + 1) + " samples");
  }
  return out;
}

ConditionalScalarSamples monteCarloConditionalScalarSamples(
  const GenericCircuit &gc, gate_t root, gate_t event_root, unsigned samples)
{
  std::mt19937_64 rng = seedRng();
  Sampler sampler(gc, rng);

  ConditionalScalarSamples out;
  out.attempted = 0;
  out.accepted.reserve(samples);

  for(unsigned i = 0; i < samples; ++i) {
    sampler.resetIteration();
    /* Evaluate the indicator FIRST: this populates bool_cache_ AND
     * scalar_cache_ for every gate_rv / gate_input that the event
     * touches, so the subsequent evalScalar(root) reads the same
     * draws.  Shared gate_t leaves between root and event_root are
     * therefore correctly coupled across the indicator and the
     * value. */
    if(sampler.evalBool(event_root)) {
      out.accepted.push_back(sampler.evalScalar(root));
    }
    ++out.attempted;

    if(provsql_interrupted)
      throw CircuitException(
              "Interrupted after " + std::to_string(i + 1) + " samples");
  }
  return out;
}

namespace {

/**
 * @brief Inverse standard-normal CDF, Beasley-Springer-Moro (1995).
 *
 * Returns @c z such that @f$\Phi(z) = p@f$.  Accurate to about
 * @c 1e-7 over @c p ∈ [0.02425, 1 - 0.02425], with a tail
 * rational fallback for the rest of @c (0, 1).  Callers must clamp
 * @c p strictly inside @c (0, 1) since the function diverges at the
 * endpoints; the truncated-normal sampler below clamps to
 * @c [1e-15, 1 - 1e-15] before each call.
 *
 * Used by @c try_truncated_closed_form_sample to invert the normal
 * CDF for inverse-CDF transform sampling.  The Beasley-Springer-Moro
 * routine is in widespread library use (NumPy/SciPy 'norminv', etc.)
 * and its accuracy is several orders of magnitude tighter than the
 * sampling noise the tests can detect at 10k draws, so it's a
 * comfortable margin.
 */
double inv_phi(double p)
{
  static const double a[] = {
    -3.969683028665376e+01,  2.209460984245205e+02,
    -2.759285104469687e+02,  1.383577518672690e+02,
    -3.066479806614716e+01,  2.506628277459239e+00
  };
  static const double b[] = {
    -5.447609879822406e+01,  1.615858368580409e+02,
    -1.556989798598866e+02,  6.680131188771972e+01,
    -1.328068155288572e+01
  };
  static const double c_arr[] = {
    -7.784894002430293e-03, -3.223964580411365e-01,
    -2.400758277161838e+00, -2.549732539343734e+00,
     4.374664141464968e+00,  2.938163982698783e+00
  };
  static const double d[] = {
     7.784695709041462e-03,  3.224671290700398e-01,
     2.445134137142996e+00,  3.754408661907416e+00
  };
  static const double p_low  = 0.02425;
  static const double p_high = 1.0 - p_low;

  if (p < p_low) {
    const double q = std::sqrt(-2.0 * std::log(p));
    return (((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
              + c_arr[3])*q + c_arr[4])*q + c_arr[5])
         / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
  }
  if (p <= p_high) {
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0]*r + a[1])*r + a[2])*r + a[3])*r + a[4])*r + a[5]) * q
         / (((((b[0]*r + b[1])*r + b[2])*r + b[3])*r + b[4])*r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - p));
  return -(((((c_arr[0]*q + c_arr[1])*q + c_arr[2])*q
             + c_arr[3])*q + c_arr[4])*q + c_arr[5])
        / ((((d[0]*q + d[1])*q + d[2])*q + d[3])*q + 1.0);
}

}  // namespace

std::optional<std::vector<double>>
try_truncated_closed_form_sample(const GenericCircuit &gc, gate_t root,
                                 gate_t event_root, unsigned n)
{
  auto m = matchTruncatedSingleRv(gc, root, event_root);
  if (!m) return std::nullopt;
  const DistributionSpec &spec = m->spec;
  const double lo = m->lo, hi = m->hi;

  std::mt19937_64 rng = seedRng();
  std::uniform_real_distribution<double> U01(0.0, 1.0);

  std::vector<double> out;
  out.reserve(n);

  switch (spec.kind) {
    case DistKind::Uniform: {
      /* @c matchTruncatedSingleRv already intersected the event's
       * interval with the RV's natural [a, b] support, so a plain
       * uniform draw on [lo, hi] is the conditional distribution. */
      std::uniform_real_distribution<double> U(lo, hi);
      for (unsigned i = 0; i < n; ++i) out.push_back(U(rng));
      return out;
    }
    case DistKind::Exponential: {
      const double lambda = spec.p1;
      if (!(lambda > 0.0)) return std::nullopt;
      if (std::isinf(hi)) {
        /* X | X > lo = lo + Exp(λ) by memorylessness.  Numerically
         * stable for arbitrarily large @c lo, where the inverse-CDF
         * would underflow on @c 1 - exp(-λ·lo). */
        std::exponential_distribution<double> E(lambda);
        for (unsigned i = 0; i < n; ++i) out.push_back(lo + E(rng));
        return out;
      }
      /* Two-sided truncation: inverse-CDF on @c [F(lo), F(hi)] with
       * @c F(x) = -expm1(-λx) for accuracy near 0, and @c x =
       * -log1p(-u)/λ for accuracy as @c u approaches 1. */
      const double F_lo = -std::expm1(-lambda * lo);
      const double F_hi = -std::expm1(-lambda * hi);
      if (!(F_lo < F_hi)) return std::nullopt;
      for (unsigned i = 0; i < n; ++i) {
        const double u = F_lo + U01(rng) * (F_hi - F_lo);
        out.push_back(-std::log1p(-u) / lambda);
      }
      return out;
    }
    case DistKind::Normal: {
      const double mu    = spec.p1;
      const double sigma = spec.p2;
      if (!(sigma > 0.0)) return std::nullopt;
      const double sqrt2 = std::sqrt(2.0);
      const double alpha = (lo - mu) / sigma;
      const double beta  = (hi - mu) / sigma;
      const double Phi_a = std::isfinite(alpha)
        ? 0.5 * (1.0 + std::erf(alpha / sqrt2))
        : (alpha < 0 ? 0.0 : 1.0);
      const double Phi_b = std::isfinite(beta)
        ? 0.5 * (1.0 + std::erf(beta / sqrt2))
        : (beta < 0 ? 0.0 : 1.0);
      if (!(Phi_a < Phi_b)) return std::nullopt;
      /* Clamp the target probability strictly inside (0, 1) so
       * @c inv_phi does not diverge near the asymptotes.  The 1e-15
       * margin is well below the BSM approximation's intrinsic
       * accuracy floor (~1e-7), so it's a safe sentinel. */
      static constexpr double EPS = 1e-15;
      for (unsigned i = 0; i < n; ++i) {
        double u = Phi_a + U01(rng) * (Phi_b - Phi_a);
        if (u < EPS)        u = EPS;
        if (u > 1.0 - EPS)  u = 1.0 - EPS;
        const double z = inv_phi(u);
        out.push_back(mu + sigma * z);
      }
      return out;
    }
    case DistKind::Erlang:
      /* Truncated Erlang requires the regularised lower incomplete
       * gamma's inverse.  Out of scope for v1; MC fallback handles it. */
      return std::nullopt;
  }
  return std::nullopt;
}

bool circuitHasRV(const GenericCircuit &gc, gate_t root)
{
  std::unordered_set<gate_t> seen;
  std::stack<gate_t> stack;
  stack.push(root);
  while(!stack.empty()) {
    gate_t g = stack.top();
    stack.pop();
    if(!seen.insert(g).second) continue;
    auto type = gc.getGateType(g);
    // gate_arith too: it's exclusively a continuous-arithmetic
    // composite, the BoolExpr semiring path has no rule for it.
    // gate_mixture is structurally a compound RV root as well.
    if(type == gate_rv || type == gate_arith || type == gate_mixture)
      return true;
    for(gate_t c : gc.getWires(g)) stack.push(c);
  }
  return false;
}

}  // namespace provsql
