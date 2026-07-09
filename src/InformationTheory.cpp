/**
 * @file InformationTheory.cpp
 * @brief Entropy / KL divergence / mutual information readouts.
 */
#include "InformationTheory.h"

#include "CircuitFromMMap.h"
#include "ConjugatePosterior.h" // conjugatePosterior (observe-evidence entropy)
#include "Expectation.h"        // lift_conditioning, evaluateBooleanProbability
#include "MonteCarloSampler.h"
#include "PivotIntegration.h"   // simpsonIntegrate, kSimpsonPanels
#include "RandomVariable.h"     // parse_distribution_spec
#include "distributions/Distribution.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "provsql_utils.h"
#include "provsql_error.h"

PG_FUNCTION_INFO_V1(rv_entropy);
PG_FUNCTION_INFO_V1(rv_kl);
PG_FUNCTION_INFO_V1(rv_mutual_information);
}

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace provsql {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

unsigned mc_budget_or_throw(const std::string &what)
{
  const int n = provsql_rv_mc_samples;
  if (n <= 0)
    throw CircuitException(
      what + " has no closed form for this shape and "
      "provsql.rv_mc_samples = 0 disables the Monte Carlo fallback");
  return static_cast<unsigned>(n);
}

/* ------------------------------------------------------------------
 * Density views.
 *
 * A gate resolves to a closed density view when its distribution is
 * expressible in one of two exact forms: a finite pmf (gate_value,
 * categorical mixture) or a continuous pdf callable over a finite
 * integration window (bare gate_rv, and Bernoulli mixture trees over
 * INDEPENDENT arms -- the gmm cascade -- whose pdf is the weighted sum
 * of the arms').  Everything else (arith composites, shared-leaf
 * mixtures, conditioned roots) has no closed view and the caller falls
 * back to MC or raises.
 * ------------------------------------------------------------------ */
struct DensityView {
  bool discrete;
  std::map<double, double> pmf;             /* discrete */
  std::function<double(double)> pdf;        /* continuous */
  double lo = 0.0, hi = 0.0;                /* continuous window */
};

/* Stochastic leaves reachable from g (gate_rv / gate_input /
 * gate_mulinput): the footprint used for the structural-independence
 * shortcuts.  Deterministic gate_value leaves are deliberately not
 * collected -- they are shared by v5-keying and carry no randomness. */
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

std::optional<DensityView>
resolveDensity(const GenericCircuit &gc, gate_t g)
{
  const auto t = gc.getGateType(g);

  if (t == gate_value) {
    double v;
    try { v = parseDoubleStrict(gc.getExtra(g)); }
    catch (const CircuitException &) { return std::nullopt; }
    DensityView view;
    view.discrete = true;
    view.pmf[v] = 1.0;
    return view;
  }

  if (t == gate_rv) {
    auto spec = parse_distribution_spec(gc.getExtra(g));
    if (!spec) return std::nullopt;
    std::shared_ptr<Distribution> dist = makeDistribution(*spec);
    DensityView view;
    view.discrete = false;
    if (!dist->integrationRange(view.lo, view.hi)) return std::nullopt;
    view.pdf = [dist](double x) { return dist->pdf(x); };
    return view;
  }

  if (t == gate_mixture) {
    const auto &wires = gc.getWires(g);
    if (gc.isCategoricalMixture(g)) {
      DensityView view;
      view.discrete = true;
      for (std::size_t i = 1; i < wires.size(); ++i) {
        double v;
        try { v = parseDoubleStrict(gc.getExtra(wires[i])); }
        catch (const CircuitException &) { return std::nullopt; }
        view.pmf[v] += gc.getProb(wires[i]);
      }
      return view;
    }
    /* Bernoulli mixture(p, x, y).  The selection coin must be a bare
     * pinned input and the arms must be leaf-disjoint (a shared leaf
     * makes the arms correlated, so the pdf is no longer the weighted
     * sum of the arm pdfs). */
    if (wires.size() != 3) return std::nullopt;
    if (gc.getGateType(wires[0]) != gate_input) return std::nullopt;
    const double pi = gc.getProb(wires[0]);
    if (std::isnan(pi)) return std::nullopt;
    auto ax = stochasticLeaves(gc, wires[1]);
    auto ay = stochasticLeaves(gc, wires[2]);
    for (gate_t l : ax)
      if (ay.count(l)) return std::nullopt;
    auto vx = resolveDensity(gc, wires[1]);
    auto vy = resolveDensity(gc, wires[2]);
    if (!vx || !vy) return std::nullopt;
    if (vx->discrete && vy->discrete) {
      DensityView view;
      view.discrete = true;
      for (const auto &[v, p] : vx->pmf) view.pmf[v] += pi * p;
      for (const auto &[v, p] : vy->pmf) view.pmf[v] += (1.0 - pi) * p;
      return view;
    }
    if (!vx->discrete && !vy->discrete) {
      DensityView view;
      view.discrete = false;
      view.lo = std::min(vx->lo, vy->lo);
      view.hi = std::max(vx->hi, vy->hi);
      auto fx = vx->pdf, fy = vy->pdf;
      view.pdf = [pi, fx, fy](double x) {
        const double a = fx(x), b = fy(x);
        if (std::isnan(a) || std::isnan(b))
          return std::numeric_limits<double>::quiet_NaN();
        return pi * a + (1.0 - pi) * b;
      };
      return view;
    }
    /* Mixed discrete/continuous mixture: neither a pmf nor a pdf. */
    return std::nullopt;
  }

  return std::nullopt;
}

/* Shannon entropy of a pmf / differential entropy of a pdf, in nats. */
double entropyOfView(const DensityView &view)
{
  if (view.discrete) {
    double h = 0.0;
    for (const auto &[v, p] : view.pmf)
      if (p > 0.0) h -= p * std::log(p);
    return h;
  }
  return simpsonIntegrate(view.lo, view.hi, kSimpsonPanels,
    [&](double x) {
      const double f = view.pdf(x);
      if (std::isnan(f)) return std::numeric_limits<double>::quiet_NaN();
      if (f <= 0.0) return 0.0;          /* x ln x -> 0 as x -> 0 */
      return -f * std::log(f);
    });
}

/* Histogram plug-in estimate of the (differential) entropy from raw
 * draws: Scott-rule bin width, H^ = -sum (n_i/m) ln(n_i / (m h)).  A
 * degenerate sample (zero spread) is a point mass: Shannon entropy 0. */
double entropyFromSamples(std::vector<double> xs, const std::string &what)
{
  xs.erase(std::remove_if(xs.begin(), xs.end(),
                          [](double v) { return std::isnan(v); }),
           xs.end());
  const std::size_t m = xs.size();
  if (m == 0)
    throw CircuitException(what + ": no defined Monte Carlo draws");
  double mean = 0.0;
  for (double v : xs) mean += v;
  mean /= static_cast<double>(m);
  double var = 0.0;
  for (double v : xs) var += (v - mean) * (v - mean);
  var /= static_cast<double>(m);
  const double sd = std::sqrt(var);
  if (!(sd > 0.0)) return 0.0;           /* point mass */
  const double h =
    3.49 * sd / std::cbrt(static_cast<double>(m));   /* Scott's rule */
  const auto [mn_it, mx_it] = std::minmax_element(xs.begin(), xs.end());
  const double lo = *mn_it, hi = *mx_it;
  const std::size_t nbins =
    std::max<std::size_t>(1, static_cast<std::size_t>((hi - lo) / h) + 1);
  std::vector<std::size_t> counts(nbins, 0);
  for (double v : xs) {
    std::size_t b = static_cast<std::size_t>((v - lo) / h);
    if (b >= nbins) b = nbins - 1;
    ++counts[b];
  }
  double H = 0.0;
  for (std::size_t c : counts) {
    if (c == 0) continue;
    const double p = static_cast<double>(c) / static_cast<double>(m);
    H -= p * std::log(p / h);
  }
  return H;
}

}  // namespace

double computeEntropy(const GenericCircuit &gc, gate_t root,
                      std::optional<gate_t> event)
{
  if (!event) {
    if (auto view = resolveDensity(gc, root)) {
      const double h = entropyOfView(*view);
      if (!std::isnan(h)) return h;
    }
    return entropyFromSamples(
      monteCarloScalarSamples(gc, root, mc_budget_or_throw("entropy")),
      "entropy");
  }
  /* Conjugate observe-evidence: the posterior is a bare distribution of
   * the prior's family, so its (differential) entropy is the exact
   * quadrature over the family pdf -- no sampling. */
  if (auto post = conjugatePosterior(gc, root, *event)) {
    std::shared_ptr<Distribution> dist = makeDistribution(*post);
    DensityView view;
    view.discrete = false;
    if (dist->integrationRange(view.lo, view.hi)) {
      view.pdf = [dist](double x) { return dist->pdf(x); };
      const double h = entropyOfView(view);
      if (!std::isnan(h)) return h;
    }
  }
  /* Conditional entropy of X | A: plug-in estimate over the rejection
   * sampler's conditional draws (the coupled joint circuit keeps a leaf
   * shared between root and event on one draw per iteration). */
  auto cs = monteCarloConditionalScalarSamples(
    gc, root, *event, mc_budget_or_throw("conditional entropy"));
  if (cs.accepted.empty())
    throw CircuitException(
      "conditional entropy: the conditioning event was never satisfied "
      "in the Monte Carlo pass (probability ~ 0?)");
  return entropyFromSamples(std::move(cs.accepted), "conditional entropy");
}

double computeKL(const GenericCircuit &gc, gate_t p_root, gate_t q_root)
{
  auto vp = resolveDensity(gc, p_root);
  auto vq = resolveDensity(gc, q_root);
  if (!vp || !vq)
    throw CircuitException(
      "kl: both arguments must resolve to a closed-form density (a bare "
      "distribution constructor, a constant, a categorical, or a mixture "
      "of those with independent arms); arithmetic composites and "
      "conditioned variables are not supported");

  if (vp->discrete && vq->discrete) {
    double kl = 0.0;
    for (const auto &[v, p] : vp->pmf) {
      if (p <= 0.0) continue;
      auto it = vq->pmf.find(v);
      const double q = (it == vq->pmf.end()) ? 0.0 : it->second;
      if (q <= 0.0) return kInf;        /* P not abs. continuous w.r.t. Q */
      kl += p * std::log(p / q);
    }
    return kl;
  }
  if (vp->discrete != vq->discrete)
    return kInf;   /* atoms vs a density: never absolutely continuous */

  /* Continuous-continuous: integrate f_p ln(f_p / f_q) over P's window.
   * f_q underflowing to 0 where f_p has mass yields +Infinity, which
   * flows through the quadrature. */
  return simpsonIntegrate(vp->lo, vp->hi, kSimpsonPanels,
    [&](double x) {
      const double fp = vp->pdf(x);
      if (std::isnan(fp)) return std::numeric_limits<double>::quiet_NaN();
      if (fp <= 0.0) return 0.0;
      const double fq = vq->pdf(x);
      if (std::isnan(fq)) return std::numeric_limits<double>::quiet_NaN();
      if (fq <= 0.0) return kInf;
      return fp * std::log(fp / fq);
    });
}

double computeMutualInformation(const GenericCircuit &gc, gate_t x_root,
                                gate_t y_root)
{
  if (x_root == y_root) {
    /* I(X; X) = H(X): finite (Shannon) for a discrete X, +Infinity for a
     * continuous one. */
    if (auto view = resolveDensity(gc, x_root))
      if (view->discrete) return entropyOfView(*view);
    return kInf;
  }

  /* Structural independence: disjoint stochastic leaves => I = 0. */
  {
    auto lx = stochasticLeaves(gc, x_root);
    auto ly = stochasticLeaves(gc, y_root);
    bool shared = false;
    for (gate_t l : lx)
      if (ly.count(l)) { shared = true; break; }
    if (!shared) return 0.0;
  }

  /* Correlated pair: 2-D histogram plug-in over coupled joint draws.
   * B ~ m^(1/3) bins per axis; I^ = sum p_ij ln(p_ij / (p_i q_j)),
   * non-negative by construction. */
  const unsigned m0 = mc_budget_or_throw("mutual_information");
  auto [xs, ys] = monteCarloScalarPairSamples(gc, x_root, y_root, m0);
  std::vector<std::pair<double, double>> pairs;
  pairs.reserve(xs.size());
  for (std::size_t i = 0; i < xs.size(); ++i)
    if (!std::isnan(xs[i]) && !std::isnan(ys[i]))
      pairs.emplace_back(xs[i], ys[i]);
  const std::size_t m = pairs.size();
  if (m == 0)
    throw CircuitException("mutual_information: no defined Monte Carlo draws");

  double xlo = pairs[0].first, xhi = xlo, ylo = pairs[0].second, yhi = ylo;
  for (const auto &[a, b] : pairs) {
    xlo = std::min(xlo, a); xhi = std::max(xhi, a);
    ylo = std::min(ylo, b); yhi = std::max(yhi, b);
  }
  if (!(xhi > xlo) || !(yhi > ylo))
    return 0.0;   /* a degenerate marginal carries no information */

  const std::size_t B = std::max<std::size_t>(
    4, static_cast<std::size_t>(std::cbrt(static_cast<double>(m))));
  auto binOf = [B](double v, double lo, double hi) {
    auto b = static_cast<std::size_t>((v - lo) / (hi - lo)
                                      * static_cast<double>(B));
    return (b >= B) ? B - 1 : b;
  };
  std::vector<std::size_t> joint(B * B, 0), margx(B, 0), margy(B, 0);
  for (const auto &[a, b] : pairs) {
    const std::size_t bx = binOf(a, xlo, xhi), by = binOf(b, ylo, yhi);
    ++joint[bx * B + by];
    ++margx[bx];
    ++margy[by];
  }
  const double dm = static_cast<double>(m);
  double mi = 0.0;
  for (std::size_t bx = 0; bx < B; ++bx)
    for (std::size_t by = 0; by < B; ++by) {
      const std::size_t c = joint[bx * B + by];
      if (c == 0) continue;
      const double pij = static_cast<double>(c) / dm;
      const double px = static_cast<double>(margx[bx]) / dm;
      const double py = static_cast<double>(margy[by]) / dm;
      mi += pij * std::log(pij / (px * py));
    }
  return std::max(mi, 0.0);
}

}  // namespace provsql

/* ------------------------------------------------------------------
 * SQL-callable entry points, mirroring rv_moment's plumbing.
 * ------------------------------------------------------------------ */

/**
 * @brief SQL: rv_entropy(token uuid, prov uuid) -> float8
 */
Datum rv_entropy(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    pg_uuid_t *prov = PG_GETARG_UUID_P(1);

    gate_t root_gate, event_gate;
    auto gc = getJointCircuit(*token, *prov, root_gate, event_gate);

    std::optional<gate_t> event_opt;
    if (gc.getGateType(event_gate) != gate_one)
      event_opt = event_gate;
    root_gate = provsql::lift_conditioning(gc, root_gate, event_opt);

    return Float8GetDatum(
      provsql::computeEntropy(gc, root_gate, event_opt));
  } catch (const std::exception &e) {
    provsql_error("rv_entropy: %s", e.what());
  } catch (...) {
    provsql_error("rv_entropy: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief SQL: rv_kl(p uuid, q uuid) -> float8
 */
Datum rv_kl(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *p = PG_GETARG_UUID_P(0);
    pg_uuid_t *q = PG_GETARG_UUID_P(1);

    gate_t p_gate, q_gate;
    auto gc = getJointCircuit(*p, *q, p_gate, q_gate);

    return Float8GetDatum(provsql::computeKL(gc, p_gate, q_gate));
  } catch (const std::exception &e) {
    provsql_error("rv_kl: %s", e.what());
  } catch (...) {
    provsql_error("rv_kl: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief SQL: rv_mutual_information(x uuid, y uuid) -> float8
 */
Datum rv_mutual_information(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *x = PG_GETARG_UUID_P(0);
    pg_uuid_t *y = PG_GETARG_UUID_P(1);

    gate_t x_gate, y_gate;
    auto gc = getJointCircuit(*x, *y, x_gate, y_gate);

    return Float8GetDatum(
      provsql::computeMutualInformation(gc, x_gate, y_gate));
  } catch (const std::exception &e) {
    provsql_error("rv_mutual_information: %s", e.what());
  } catch (...) {
    provsql_error("rv_mutual_information: unknown exception");
  }
  PG_RETURN_NULL();
}
