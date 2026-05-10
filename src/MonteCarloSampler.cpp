/**
 * @file MonteCarloSampler.cpp
 * @brief Implementation of the RV-aware Monte Carlo sampler.
 */
#include "MonteCarloSampler.h"
#include "Aggregation.h"
#include "RandomVariable.h"
#include "Circuit.h"

#include <cstdint>
#include <random>
#include <stack>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    if(type == gate_rv || type == gate_arith) return true;
    for(gate_t c : gc.getWires(g)) stack.push(c);
  }
  return false;
}

}  // namespace provsql
