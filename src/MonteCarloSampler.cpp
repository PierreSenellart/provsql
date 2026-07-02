/**
 * @file MonteCarloSampler.cpp
 * @brief Implementation of the RV-aware Monte Carlo sampler.
 */
#include "MonteCarloSampler.h"
#include "Aggregation.h"
#include "RandomVariable.h"
#include "distributions/Distribution.h"  // makeDistribution -> per-family sample()
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
  // Per-gate_rv Distribution, constructed once and reused across iterations
  // (NOT cleared in resetIteration): sampling then never re-parses the spec
  // or re-constructs the Distribution per draw.
  std::unordered_map<gate_t, std::unique_ptr<Distribution>> dist_cache_;
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
    case gate_assumed:
      // Structural Boolean-rewrite marker: identity on the Boolean
      // semiring, so the sampled truth value is the wrapped child's.
      // The marker exists to refuse non-Boolean-compat evaluation; MC
      // sampling for probability is always Boolean-compat.
      if(wires.size() != 1)
        throw CircuitException(
                "gate_assumed must have exactly one child");
      result = evalBool(wires[0]);
      break;
    case gate_annotation:
      // Transparent annotation wrapper (inversion-free certificate / order
      // key): identity, so the sampled truth value is the wrapped child's.
      if(wires.size() != 1)
        throw CircuitException("gate_annotation must have exactly one child");
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
      auto dit = dist_cache_.find(g);
      if(dit == dist_cache_.end()) {
        auto spec = parse_distribution_spec(gc_.getExtra(g));
        if(!spec)
          throw CircuitException(
                  "Malformed gate_rv extra: " + gc_.getExtra(g));
        dit = dist_cache_.emplace(g, makeDistribution(*spec)).first;
      }
      result = dit->second->sample(rng_);
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
        case PROVSQL_ARITH_MAX:
          // n-ary order statistic: max over the sampled children.  Shared
          // base RVs stay coupled through rv_cache_, so max(x, y) with x,y
          // over the same leaf draws them jointly (correct correlation).
          result = evalScalar(wires[0]);
          for(std::size_t i = 1; i < wires.size(); ++i)
            result = std::max(result, evalScalar(wires[i]));
          break;
        case PROVSQL_ARITH_MIN:
          result = evalScalar(wires[0]);
          for(std::size_t i = 1; i < wires.size(); ++i)
            result = std::min(result, evalScalar(wires[i]));
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
      // makeAggregator to SumAgg<long>, so each kept row contributes its
      // value gate cast to long: that gate is 1 for an ordinary row and 0
      // for a NULL one (count(x) does not count NULLs), so the sum of the
      // kept values is exactly count(*) / count(x) -- faithful with no
      // nullability check.  SUM / AVG / MIN / MAX consume the value via
      // evalScalar directly.  Empty groups finalise to NONE; SUM / COUNT
      // surface 0 (the additive identity, the ProvSQL empty-group
      // convention) and AVG / MIN / MAX surface NaN, which compares false
      // under IEEE on any subsequent gate_cmp -- the SQL convention for
      // HAVING on an empty group.
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
          agg->add(AggValue(static_cast<long>(evalScalar(sm[1]))));
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
          // ProvSQL convention diverges from standard SQL here: COUNT
          // and SUM over an empty group both yield the additive
          // identity 0 (see test/sql/continuous_aggregation.sql §5 --
          // empty-group SUM returns as_random(0) as a deterministic
          // Dirac).  MC sampling mirrors that so an iteration where
          // every semimod's Boolean filter was false produces 0.0
          // rather than poisoning the moment estimator with NaN.
          // MIN / MAX / AVG over empty groups stay NaN: there is no
          // natural identity for those, and the moment averagers in
          // Expectation::mc_raw_moment / mc_central_moment skip NaN
          // samples so the estimator is conditional on non-empty
          // worlds.
          result = (op == AggregationOperator::COUNT
                    || op == AggregationOperator::SUM)
                   ? 0.0
                   : std::numeric_limits<double>::quiet_NaN();
          break;
        default:
          throw CircuitException(
                  "gate_agg: unsupported aggregate result ValueType in MC");
      }
      break;
    }
    case gate_semimod:
    {
      // Bare semimod root (the user pinned one of an agg's per-row
      // contributions): interpret as a Bernoulli-weighted scalar
      // value · 1_{k fires}.  When the Boolean k child does not fire
      // in this world, the row contributes nothing -- return 0.0
      // (the additive identity), which matches the per-iteration
      // role semimod plays inside gate_agg above.  This makes
      // semimod a legal scalar root for rv_sample / rv_moment /
      // rv_histogram alongside agg.
      const auto &wires = gc_.getWires(g);
      if(wires.size() != 2)
        throw CircuitException(
                "gate_semimod must have exactly two children "
                "[k_gate, value_gate]");
      result = evalBool(wires[0]) ? evalScalar(wires[1]) : 0.0;
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
    case gate_case:
    {
      // Guarded selection [g_1, v_1, ..., g_k, v_k, default] (2k+1 wires):
      // first-match on the current draw.  Evaluating the guards through
      // evalBool and the values through evalScalar in the same iteration
      // keeps shared base RVs coupled (a value and a guard over the same leaf
      // draw jointly), which is exactly why gate_case beats a mixture-of-
      // conditioned lowering that would resample and lose the correlation.
      const auto &wires = gc_.getWires(g);
      if(wires.empty())
        throw CircuitException(
                "gate_case must have at least one child (the default)");
      const std::size_t k = wires.size() / 2;
      bool matched = false;
      for(std::size_t i = 0; i < k; ++i) {
        if(evalBool(wires[2 * i])) {
          result = evalScalar(wires[2 * i + 1]);
          matched = true;
          break;
        }
      }
      if(!matched)
        result = evalScalar(wires.back());  // the default value
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

double monteCarloRVStopping(const GenericCircuit &gc, gate_t root,
                            double eps, double delta,
                            unsigned long max_samples,
                            unsigned long &samples_used,
                            bool &reached_target)
{
  samples_used = 0;
  reached_target = false;
  if(max_samples == 0)
    return 0.;

  // DKLR stopping threshold on the success count -- the S=1 Bernoulli case of
  // BooleanCircuit::karpLubyStopping: draw whole-circuit worlds until the
  // success count reaches Y1 and return Y1/N, a relative (eps,delta) estimate
  // of Pr[root]; N adapts to the true Pr[root] (expected Y1/Pr[root]).
  const double e  = std::exp(1.0);
  const double Y  = 4.0 * (e - 2.0) * std::log(2.0 / delta) / (eps * eps);
  const double Y1 = 1.0 + (1.0 + eps) * Y;

  std::mt19937_64 rng = seedRng();
  Sampler sampler(gc, rng);

  unsigned long success = 0;
  for(unsigned long s = 0; s < max_samples; ++s) {
    sampler.resetIteration();
    if(sampler.evalBool(root)) {
      ++success;
      if(static_cast<double>(success) >= Y1) {
        samples_used = s + 1;
        reached_target = true;
        return Y1 / static_cast<double>(samples_used);
      }
    }
    if(provsql_interrupted)
      throw CircuitException(
              "Interrupted after " + std::to_string(s + 1) + " samples");
  }

  // Cap reached before the threshold: the relative target is not met, so return
  // the plain unbiased mean over the spent budget.
  samples_used = max_samples;
  return static_cast<double>(success) / static_cast<double>(max_samples);
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

std::optional<std::vector<double>>
try_truncated_closed_form_sample(const GenericCircuit &gc, gate_t root,
                                 gate_t event_root, unsigned n)
{
  auto m = matchTruncatedSingleRv(gc, root, event_root);
  if (!m) return std::nullopt;

  /* Per-family rejection-free scheme (Distribution::sampleTruncated);
   * a family without one (Erlang: needs the inverse regularised
   * incomplete gamma) returns nullopt and the MC-rejection fallback
   * handles it. */
  std::mt19937_64 rng = seedRng();
  return makeDistribution(m->spec)->sampleTruncated(rng, m->lo, m->hi, n);
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
    // A continuous random variable is signalled by a gate_rv leaf or a
    // gate_mixture root.  gate_arith is NOT itself an RV marker: it is also
    // arithmetic over aggregates (resolved by provsql_having's possible-worlds
    // enumeration).  A genuine RV arithmetic gate_arith still reaches its
    // gate_rv leaves through the child walk below, so it is caught.
    if(type == gate_rv || type == gate_mixture)
      return true;
    for(gate_t c : gc.getWires(g)) stack.push(c);
  }
  return false;
}

bool circuitHasUnresolvedSampleableAgg(const GenericCircuit &gc, gate_t root)
{
  // True iff a gate_agg survives the probability pre-passes AND every surviving
  // one is sample-faithful: SUM / AVG / MIN / MAX / COUNT -- all the aggregates
  // the sampler's gate_agg arm reproduces exactly.  That arm pushes each kept
  // contributor's value into the matching Aggregator: the value gate is the
  // row's contribution (the summed term for SUM; the 0/1 indicator for COUNT,
  // 0 for a NULL row so count(x) does not count NULLs; the compared value for
  // AVG / MIN / MAX), so NULL rows are handled and an empty group finalises to
  // the value the exact HAVING evaluator uses (0 for SUM / COUNT, NaN ->
  // comparison false for AVG / MIN / MAX).  An aggregate that bailed the exact
  // evaluators (whose threshold-lineage expansion would otherwise not terminate
  // for a large-magnitude / large-support aggregate) is then estimated by direct
  // world sampling: the apx-safe corner of the HAVING trichotomy (Re & Suciu).
  // gate_arith over such aggregates is covered (its gate_agg leaves are reached
  // by the walk).  The explicit switch rejects any future aggregate operator the
  // sampler does not yet handle.
  std::unordered_set<gate_t> seen;
  std::stack<gate_t> stack;
  stack.push(root);
  bool any = false;
  while(!stack.empty()) {
    gate_t g = stack.top();
    stack.pop();
    if(!seen.insert(g).second) continue;
    if(gc.getGateType(g) == gate_agg) {
      switch(getAggregationOperator(gc.getInfos(g).first)) {
      case AggregationOperator::SUM:
      case AggregationOperator::AVG:
      case AggregationOperator::MIN:
      case AggregationOperator::MAX:
      case AggregationOperator::COUNT:
        any = true;
        break;
      default:                       // an aggregate the sampler lacks: not routed
        return false;
      }
    }
    for(gate_t c : gc.getWires(g)) stack.push(c);
  }
  return any;
}

}  // namespace provsql
