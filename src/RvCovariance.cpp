/**
 * @file RvCovariance.cpp
 * @brief Single-pass covariance / correlation readouts over RV circuits.
 *
 * Backs the SQL @c covariance(x, y[, prov]) and @c correlation(x, y[, prov])
 * readouts.  The definitional decomposition
 * @f$Cov(X, Y) = E[XY] - E[X]\,E[Y]@f$ is kept for the exact cases (every
 * factor decomposes analytically, so the subtraction is closed-form), but
 * the Monte Carlo fallback deliberately does NOT go through it: estimating
 * @c E[XY] and the two means from three independent sampling passes and
 * subtracting puts the estimator's noise on the scale of
 * @f$E[X]\,E[Y]@f$ -- the product of the means -- rather than of the
 * covariance itself, a catastrophic cancellation when the means dominate
 * the coupling.  Instead a single coupled pass draws @f$(x_i, y_i)@f$ pairs
 * from the joint circuit (shared stochastic leaves produce one draw per
 * iteration that both roots observe) and returns the plain sample
 * covariance @f$\tfrac1n \sum (x_i - \bar x)(y_i - \bar y)@f$, whose noise
 * scales with @f$\sqrt{(\sigma_X^2 \sigma_Y^2 + Cov^2)/n}@f$.
 *
 * @c correlation reads @f$Cov@f$, @f$\sigma_X@f$ and @f$\sigma_Y@f$ off the
 * SAME pass, instead of stacking five independent MC estimates.
 */
#include "CircuitFromMMap.h"
#include "Expectation.h"
#include "MonteCarloSampler.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_covariance);
PG_FUNCTION_INFO_V1(rv_correlation);
}

#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace provsql {

namespace {

/* Stochastic leaves reachable from g, as in InformationTheory.cpp's
 * structural-independence shortcut: gate_value constants are shared by
 * v5-keying and carry no randomness, so they are not collected. */
void collectStochasticLeaves(const GenericCircuit &gc, gate_t g,
                             std::set<gate_t> &out, std::set<gate_t> &seen)
{
  if (!seen.insert(g).second) return;
  const auto t = gc.getGateType(g);
  if (t == gate_rv || t == gate_input || t == gate_mulinput)
    out.insert(g);
  for (gate_t c : gc.getWires(g))
    collectStochasticLeaves(gc, c, out, seen);
}

std::set<gate_t> stochasticLeaves(const GenericCircuit &gc, gate_t g)
{
  std::set<gate_t> out, seen;
  collectStochasticLeaves(gc, g, out, seen);
  return out;
}

bool disjointFrom(const std::set<gate_t> &a, const std::set<gate_t> &b)
{
  for (gate_t l : a)
    if (b.count(l)) return false;
  return true;
}

/* Scoped MC-fallback disable for the analytic attempt: with
 * provsql.rv_mc_samples forced to 0 every evaluator below throws instead
 * of sampling, so "no CircuitException" == "the result is closed-form". */
struct McDisabledGuard {
  int saved;
  McDisabledGuard() : saved(provsql_rv_mc_samples) { provsql_rv_mc_samples = 0; }
  ~McDisabledGuard() { provsql_rv_mc_samples = saved; }
};

/* In-memory product gate over the two roots, for the exact
 * E[XY] - E[X]E[Y] path.  The persistent store is never mutated. */
gate_t makeProductGate(GenericCircuit &gc, gate_t a, gate_t b)
{
  gate_t g = gc.setGate(gate_arith);
  auto &w = gc.getWires(g);
  w.push_back(a);
  w.push_back(b);
  gc.setInfos(g, PROVSQL_ARITH_TIMES, 0);
  return g;
}

/* Mirrors Expectation.cpp's mc_samples_or_throw: throw when the GUC
 * disables the fallback, emit the approximation-transparency NOTICE at
 * the same verbosity tier otherwise. */
unsigned mc_samples_or_throw(const std::string &what)
{
  const int n = provsql_rv_mc_samples;
  if (n <= 0) {
    throw CircuitException(
      what + " could not be decomposed analytically and "
      "provsql.rv_mc_samples = 0 disables the Monte Carlo fallback");
  }
  if (provsql_verbose >= 5)
    provsql_notice(
      "%s: no closed form found; estimating by Monte Carlo over %d samples "
      "(an approximation, not an exact moment) -- set provsql.rv_mc_samples = 0 "
      "to require an exact result instead", what.c_str(), n);
  return static_cast<unsigned>(n);
}

/* Acceptance guard for the conditional pass, mirroring Expectation.cpp's
 * check_acceptance_or_throw (0-of-N = infeasible event, otherwise a
 * max(attempted/1000, 5) floor against enormous-variance estimates). */
void check_acceptance_or_throw(std::size_t accepted, unsigned attempted,
                               const std::string &what)
{
  if (accepted == 0) {
    throw CircuitException(
      what + ": conditioning event is infeasible (0 of " +
      std::to_string(attempted) + " Monte Carlo samples satisfied it)");
  }
  unsigned floor = attempted / 1000;
  if (floor < 5) floor = 5;
  if (accepted < floor) {
    throw CircuitException(
      what + ": conditional MC accepted only " + std::to_string(accepted) +
      " out of " + std::to_string(attempted) +
      " samples (need >= " + std::to_string(floor) +
      "); raise provsql.rv_mc_samples or tighten the event.");
  }
}

/* Sample covariance and the two sample variances from one coupled pass.
 * Population (1/n) normalisation, matching the central-moment MC
 * convention in Expectation.cpp.  Pairs with a NaN member (sampling-
 * undefined worlds, e.g. empty-group aggregates) are excluded; an
 * all-NaN pass yields NaN throughout. */
struct CovStats {
  double cov;
  double var_x;
  double var_y;
};

CovStats mcCovStats(const GenericCircuit &gc, gate_t x, gate_t y,
                    std::optional<gate_t> event, const std::string &what)
{
  const unsigned n = mc_samples_or_throw(what);

  std::vector<double> xs, ys;
  if (event) {
    auto cs = monteCarloConditionalScalarPairSamples(gc, x, y, *event, n);
    check_acceptance_or_throw(cs.xs.size(), cs.attempted, what);
    xs = std::move(cs.xs);
    ys = std::move(cs.ys);
  } else {
    std::tie(xs, ys) = monteCarloScalarPairSamples(gc, x, y, n);
  }

  double sum_x = 0.0, sum_y = 0.0;
  std::size_t m = 0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    if (std::isnan(xs[i]) || std::isnan(ys[i])) continue;
    sum_x += xs[i];
    sum_y += ys[i];
    ++m;
  }
  if (m == 0) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return {nan, nan, nan};
  }
  const double mean_x = sum_x / static_cast<double>(m);
  const double mean_y = sum_y / static_cast<double>(m);

  double cov = 0.0, var_x = 0.0, var_y = 0.0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    if (std::isnan(xs[i]) || std::isnan(ys[i])) continue;
    const double dx = xs[i] - mean_x;
    const double dy = ys[i] - mean_y;
    cov += dx * dy;
    var_x += dx * dx;
    var_y += dy * dy;
  }
  const double dm = static_cast<double>(m);
  return {cov / dm, var_x / dm, var_y / dm};
}

/* Exact E[XY] - E[X]E[Y] (and, when @p need_vars, the two variances),
 * attempted with the MC fallback disabled: std::nullopt means some factor
 * has no closed form and the caller should take the single coupled pass
 * instead of stacking independent estimates. */
std::optional<CovStats> tryAnalyticCovStats(GenericCircuit &gc, gate_t x,
                                            gate_t y,
                                            std::optional<gate_t> event,
                                            bool need_vars)
{
  McDisabledGuard guard;
  try {
    CovStats s{0.0, 0.0, 0.0};
    const double ex = compute_expectation(gc, x, event);
    const double ey = compute_expectation(gc, y, event);
    const gate_t prod = makeProductGate(gc, x, y);
    const double exy = compute_expectation(gc, prod, event);
    s.cov = exy - ex * ey;
    if (need_vars) {
      s.var_x = compute_central_moment(gc, x, 2, event);
      s.var_y = compute_central_moment(gc, y, 2, event);
    }
    return s;
  } catch (const CircuitException &) {
    return std::nullopt;
  }
}

}  // namespace

/**
 * @brief @f$Cov(X, Y)@f$ (or @f$Cov(X, Y \mid E)@f$) over a joint circuit.
 *
 * Exact tiers first: a variance readout when the two roots coincide, an
 * exact @c 0 when the roots' stochastic-leaf footprints are structurally
 * independent (given the event), and the closed-form
 * @f$E[XY] - E[X]\,E[Y]@f$ when every factor decomposes analytically.
 * Otherwise one coupled Monte Carlo pass at the @c provsql.rv_mc_samples
 * budget.
 */
double computeCovariance(GenericCircuit &gc, gate_t x, gate_t y,
                         std::optional<gate_t> event)
{
  x = lift_conditioning(gc, x, event);
  y = lift_conditioning(gc, y, event);

  /* Cov(X, X) = Var(X): route to the central-moment evaluator (closed
   * form where the variance has one; single-root MC otherwise) instead
   * of a product gate over a shared root, which never decomposes. */
  if (x == y)
    return compute_central_moment(gc, x, 2, event);

  /* Structural independence: X independent of (Y, E) -- or symmetrically
   * Y of (X, E) -- gives Cov(X, Y | E) = 0 exactly, whatever the
   * factors' moments look like. */
  {
    auto lx = stochasticLeaves(gc, x);
    auto ly = stochasticLeaves(gc, y);
    std::set<gate_t> le;
    if (event) le = stochasticLeaves(gc, *event);
    const bool x_indep = disjointFrom(lx, ly) && disjointFrom(lx, le);
    const bool y_indep = disjointFrom(ly, lx) && disjointFrom(ly, le);
    if (x_indep || y_indep)
      return 0.0;
  }

  if (auto s = tryAnalyticCovStats(gc, x, y, event, false))
    return s->cov;

  return mcCovStats(gc, x, y, event, "covariance").cov;
}

/**
 * @brief Pearson @f$\rho(X, Y) = Cov(X, Y) / (\sigma_X \sigma_Y)@f$
 *        (conditioned on @p event when set).
 *
 * Same exact tiers as @c computeCovariance; on the Monte Carlo path the
 * covariance and BOTH standard deviations are read off one coupled pass.
 * @c std::nullopt when either variance is @c 0 (or the statistics are
 * undefined): correlation with a degenerate variable has no value, and
 * the SQL binding maps this to @c NULL.
 */
std::optional<double> computeCorrelation(GenericCircuit &gc, gate_t x,
                                         gate_t y,
                                         std::optional<gate_t> event)
{
  x = lift_conditioning(gc, x, event);
  y = lift_conditioning(gc, y, event);

  if (x == y) {
    const double var = compute_central_moment(gc, x, 2, event);
    if (std::isnan(var) || var <= 0.0) return std::nullopt;
    return 1.0;
  }

  {
    auto lx = stochasticLeaves(gc, x);
    auto ly = stochasticLeaves(gc, y);
    std::set<gate_t> le;
    if (event) le = stochasticLeaves(gc, *event);
    const bool x_indep = disjointFrom(lx, ly) && disjointFrom(lx, le);
    const bool y_indep = disjointFrom(ly, lx) && disjointFrom(ly, le);
    if (x_indep || y_indep) {
      const double var_x = compute_central_moment(gc, x, 2, event);
      const double var_y = compute_central_moment(gc, y, 2, event);
      if (std::isnan(var_x) || std::isnan(var_y) ||
          var_x <= 0.0 || var_y <= 0.0)
        return std::nullopt;
      return 0.0;
    }
  }

  CovStats s{};
  if (auto exact = tryAnalyticCovStats(gc, x, y, event, true))
    s = *exact;
  else
    s = mcCovStats(gc, x, y, event, "correlation");

  if (std::isnan(s.var_x) || std::isnan(s.var_y) ||
      s.var_x <= 0.0 || s.var_y <= 0.0)
    return std::nullopt;
  return s.cov / std::sqrt(s.var_x * s.var_y);
}

}  // namespace provsql

/* ------------------------------------------------------------------
 * SQL-callable entry points, mirroring rv_moment's plumbing.
 * ------------------------------------------------------------------ */

/**
 * @brief SQL: rv_covariance(x uuid, y uuid, prov uuid) -> float8
 */
Datum rv_covariance(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *x = PG_GETARG_UUID_P(0);
    pg_uuid_t *y = PG_GETARG_UUID_P(1);
    pg_uuid_t *prov = PG_GETARG_UUID_P(2);

    std::vector<gate_t> gates;
    auto gc = getJointCircuit({*x, *y, *prov}, gates);

    /* gate_one event = unconditional after load-time simplification. */
    std::optional<gate_t> event_opt;
    if (gc.getGateType(gates[2]) != gate_one)
      event_opt = gates[2];

    return Float8GetDatum(
      provsql::computeCovariance(gc, gates[0], gates[1], event_opt));
  } catch (const std::exception &e) {
    provsql_error("rv_covariance: %s", e.what());
  } catch (...) {
    provsql_error("rv_covariance: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief SQL: rv_correlation(x uuid, y uuid, prov uuid) -> float8
 *
 * Returns SQL @c NULL for a degenerate (zero-variance) argument, matching
 * the @c NULLIF convention of the former SQL-level definition.
 */
Datum rv_correlation(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *x = PG_GETARG_UUID_P(0);
    pg_uuid_t *y = PG_GETARG_UUID_P(1);
    pg_uuid_t *prov = PG_GETARG_UUID_P(2);

    std::vector<gate_t> gates;
    auto gc = getJointCircuit({*x, *y, *prov}, gates);

    std::optional<gate_t> event_opt;
    if (gc.getGateType(gates[2]) != gate_one)
      event_opt = gates[2];

    auto rho =
      provsql::computeCorrelation(gc, gates[0], gates[1], event_opt);
    if (!rho)
      PG_RETURN_NULL();
    return Float8GetDatum(*rho);
  } catch (const std::exception &e) {
    provsql_error("rv_correlation: %s", e.what());
  } catch (...) {
    provsql_error("rv_correlation: unknown exception");
  }
  PG_RETURN_NULL();
}
