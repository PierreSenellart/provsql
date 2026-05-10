/**
 * @file Expectation.cpp
 * @brief Implementation of the analytical expectation / variance / moment
 *        evaluator over scalar RV sub-circuits.
 */
#include "Expectation.h"

#include "Circuit.h"
#include "CircuitFromMMap.h"
#include "MonteCarloSampler.h"
#include "RandomVariable.h"
#include "provsql_utils_cpp.h"

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

namespace {

using RvSet = std::set<gate_t>;

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
    default:
      return mc_raw_moment(gc, g, k,
        "Raw moment of gate type " + std::string(gate_type_name[type]));
  }
}

}  // namespace

double compute_expectation(const GenericCircuit &gc, gate_t root)
{
  FootprintCache fp(gc);
  return rec_expectation(gc, root, fp);
}

double compute_variance(const GenericCircuit &gc, gate_t root)
{
  FootprintCache fp(gc);
  return rec_variance(gc, root, fp);
}

double compute_raw_moment(const GenericCircuit &gc, gate_t root, unsigned k)
{
  FootprintCache fp(gc);
  return rec_raw_moment(gc, root, k, fp);
}

double compute_central_moment(const GenericCircuit &gc, gate_t root, unsigned k)
{
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
 * @brief SQL: rv_moment(token uuid, k integer, central boolean) -> float8
 *
 * Single C entry point shared by the @c expected / @c variance /
 * @c moment / @c central_moment SQL functions.  The SQL wrappers
 * select the (k, central) pair that matches their semantics:
 * - @c expected(rv): k=1, central=false (or routes through
 *   @c provenance_evaluate_compiled(..., 'expectation', ...)).
 * - @c variance(rv): k=2, central=true.
 * - @c moment(rv, k): central=false.
 * - @c central_moment(rv, k): central=true.
 */
Datum rv_moment(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    const int32 k_signed = PG_GETARG_INT32(1);
    const bool central = PG_GETARG_BOOL(2);

    if (k_signed < 0)
      provsql_error("rv_moment: k must be non-negative (got %d)", k_signed);
    const unsigned k = static_cast<unsigned>(k_signed);

    auto gc = getGenericCircuit(*token);
    auto root = gc.getGate(uuid2string(*token));

    double result;
    if (central)
      result = provsql::compute_central_moment(gc, root, k);
    else if (k == 1)
      result = provsql::compute_expectation(gc, root);
    else
      result = provsql::compute_raw_moment(gc, root, k);
    return Float8GetDatum(result);
  } catch (const std::exception &e) {
    provsql_error("rv_moment: %s", e.what());
  } catch (...) {
    provsql_error("rv_moment: unknown exception");
  }
  PG_RETURN_NULL();
}

}  // extern "C"
