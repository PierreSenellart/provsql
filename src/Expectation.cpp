/**
 * @file Expectation.cpp
 * @brief Implementation of the analytical expectation / variance / moment
 *        evaluator over scalar RV sub-circuits.
 */
#include "Expectation.h"

#include "AggMarginalEvaluator.h"  // aggAvgRawMomentExact
#include "AnalyticEvaluator.h"
#include "Aggregation.h"        // ComparisonOperator + cmpOpFromOid
#include "BooleanCircuit.h"
#include "Circuit.h"
#include "CircuitFromMMap.h"
#include "MonteCarloSampler.h"
#include "RandomVariable.h"
#include "distributions/Distribution.h"  // makeDistribution -> integrationRange
#include "PivotIntegration.h"  // simpsonIntegrate / binomial / centralFromRaw
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
PG_FUNCTION_INFO_V1(rv_quantile);
PG_FUNCTION_INFO_V1(rv_evidence);
PG_FUNCTION_INFO_V1(agg_avg_moment_exact);
}

#include <algorithm>
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
  propagateDNNFCertificate(gc, gc_to_bc, c);

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

/// True iff @p g is a @c gate_rv whose distribution has a wired (token)
/// parameter -- a latent / compound leaf with no constant-parameter
/// closed form, so every analytic path must fall through to Monte Carlo.
bool rvIsParametric(const GenericCircuit &gc, gate_t g)
{
  if (gc.getGateType(g) != gate_rv) return false;
  auto tmpl = parse_distribution_template(gc.getExtra(g));
  return tmpl && tmpl->parametric();
}

/// Whether the family of @p tmpl has a mean affine in its parameters, so the
/// compound-leaf expectation is exact via @c mean(E[θ]) (see the
/// @c meanIsAffine() doc).  The flag is authoritative, per-family.
bool familyMeanIsAffine(const DistributionTemplate &tmpl)
{
  return tmpl.family->factory(0.0, 0.0)->meanIsAffine();
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
      // The leaf itself is a distinct random source (two independent
      // normal(M,1) leaves share M but draw independently given it), so
      // its own gate_t is always in the footprint.  A latent (parametric)
      // leaf ALSO carries the footprints of its parameter wires, so two
      // leaves sharing a latent M overlap on M and are correctly flagged
      // dependent -- defeating the closed-form independence shortcuts and
      // routing the whole expression to the MC path that couples them.
      // A non-parametric leaf has no wires, so this is a no-op there.
      s.insert(g);
      for (gate_t c : gc_.getWires(g)) {
        const auto &cs = of(c);
        s.insert(cs.begin(), cs.end());
      }
    } else if (type == gate_value) {
      // empty -- no RV reached
    } else if (type == gate_arith || type == gate_case) {
      // gate_arith: all children are scalar operands.  gate_case: the
      // selected value depends on both the guards (which RVs they compare)
      // and the value branches, so the footprint is the union over every
      // wire -- a value and a guard sharing a leaf make the selection
      // correlated, which must defeat the independence shortcuts.
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
            || type == gate_cmp || type == gate_update
            || type == gate_annotation || type == gate_conditioned) {
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

/* Closed-form E[max] / E[min] of a gate_arith MAX/MIN whose children are
 * independent bare gate_rv leaves of the SAME family with the SAME parameters
 * (the i.i.d. order statistic).  Returns std::nullopt when the shape is not
 * i.i.d. bare-RV (mixture-wrapped aggregate children, mixed families, shared
 * leaves, or a family without an elementary order-statistic mean) so the
 * caller falls back to Monte Carlo.
 *
 * The per-family closed forms live in @c Distribution::iidOrderStatMean
 * (Uniform, Exponential; Normal and Erlang i.i.d. maxima have no elementary
 * closed form -- they need the 1-D order-statistic quadrature -- so they
 * decline there).
 */
std::optional<double>
iidOrderStatMean(const GenericCircuit &gc, gate_t g, bool isMax,
                 FootprintCache &fp)
{
  const auto &raw_wires = gc.getWires(g);
  if (raw_wires.empty())
    return std::nullopt;
  /* Idempotence: max / min ignore repeats, so a child appearing more than
   * once (the same gate) counts once.  De-duplicating here generalises the
   * closed form to any MAX/MIN gate -- without it a repeated child would
   * collide with itself in pairwise_disjoint and silently drop to MC. */
  std::vector<gate_t> wires;
  wires.reserve(raw_wires.size());
  {
    std::set<gate_t> seen;
    for (gate_t c : raw_wires)
      if (seen.insert(c).second)
        wires.push_back(c);
  }
  if (!pairwise_disjoint(fp, wires))
    return std::nullopt;  /* shared leaves -> correlated -> MC */

  std::vector<DistributionSpec> specs;
  specs.reserve(wires.size());
  for (gate_t c : wires) {
    if (gc.getGateType(c) != gate_rv)
      return std::nullopt;  /* not a bare RV (e.g. a mixture wrap) */
    auto s = parse_distribution_spec(gc.getExtra(c));
    if (!s)
      return std::nullopt;
    specs.push_back(*s);
  }

  /* i.i.d.: identical family and parameters across all children (the
   * family descriptor is interned, so pointer comparison is family
   * identity). */
  for (std::size_t i = 1; i < specs.size(); ++i)
    if (specs[i].family != specs[0].family ||
        specs[i].p1 != specs[0].p1 || specs[i].p2 != specs[0].p2)
      return std::nullopt;

  return makeDistribution(specs[0])->iidOrderStatMean(specs.size(), isMax);
}

/* Closed-form (quadrature) E[max] / E[min] of a gate_arith MAX/MIN whose
 * children are independent bare gate_rv leaves of *any* families (mixed or
 * not-identical), generalising @c iidOrderStatMean.  Uses the layer-cake
 * identity over a window [lo, hi] covering every child's support:
 *   E[max] = lo + ∫ (1 − ∏ F_i(t)) dt,   E[min] = lo + ∫ ∏ (1 − F_i(t)) dt.
 * Composite Simpson.  std::nullopt on shared leaves, a non-bare-RV child, or
 * a distribution whose CDF is undefined (e.g. non-integer Erlang), so the
 * caller falls back to Monte Carlo. */
std::optional<double>
mixedOrderStatMean(const GenericCircuit &gc, gate_t g, bool isMax,
                   FootprintCache &fp)
{
  const auto &raw_wires = gc.getWires(g);
  if (raw_wires.empty())
    return std::nullopt;
  std::vector<gate_t> wires;
  {
    std::set<gate_t> seen;
    for (gate_t c : raw_wires)
      if (seen.insert(c).second)
        wires.push_back(c);
  }
  if (!pairwise_disjoint(fp, wires))
    return std::nullopt;

  // Construct each child's Distribution once; the Simpson loop below calls
  // cdf on them per point (never re-constructing per point).
  std::vector<std::unique_ptr<Distribution>> dists;
  double lo = 0.0, hi = 0.0;
  bool first = true;
  for (gate_t c : wires) {
    if (gc.getGateType(c) != gate_rv)
      return std::nullopt;
    auto s = parse_distribution_spec(gc.getExtra(c));
    if (!s)
      return std::nullopt;
    auto d = makeDistribution(*s);
    double clo, chi;
    if (!d->integrationRange(clo, chi))
      return std::nullopt;
    if (first) { lo = clo; hi = chi; first = false; }
    else       { lo = std::min(lo, clo); hi = std::max(hi, chi); }
    dists.push_back(std::move(d));
  }
  if (!(hi > lo))
    return std::nullopt;

  const double integral = simpsonIntegrate(lo, hi, kSimpsonPanels,
    [&](double t) {
      if (isMax) {
        double prodF = 1.0;             /* ∏ F_i(t) = P(max ≤ t) */
        for (const auto &d : dists) {
          const double F = d->cdf(t);
          if (std::isnan(F)) return std::numeric_limits<double>::quiet_NaN();
          prodF *= F;
        }
        return 1.0 - prodF;             /* P(max > t) */
      }
      double prod1mF = 1.0;             /* ∏ (1 − F_i(t)) = P(min > t) */
      for (const auto &d : dists) {
        const double F = d->cdf(t);
        if (std::isnan(F)) return std::numeric_limits<double>::quiet_NaN();
        prod1mF *= (1.0 - F);
      }
      return prod1mF;
    });
  if (std::isnan(integral))
    return std::nullopt;
  return lo + integral;
}

unsigned mc_samples_or_throw(const std::string &what)
{
  const int n = provsql_rv_mc_samples;
  if (n <= 0) {
    throw CircuitException(
      what + " could not be decomposed analytically and "
      "provsql.rv_mc_samples = 0 disables the Monte Carlo fallback");
  }
  // Transparency: the analytic moment surface is about to return a Monte Carlo
  // ESTIMATE, not a closed-form moment.  Signal it (at the same verbose>=5
  // evaluation tier as the probability-side approximation NOTICEs, so Studio
  // and verbose users can tell an estimate from an exact value) -- the
  // continuous-RV surface is approximate by nature, but never *silently* so.
  // Set provsql.rv_mc_samples = 0 to require an exact result instead.
  if (provsql_verbose >= 5)
    provsql_notice(
      "%s: no closed form found; estimating by Monte Carlo over %d samples "
      "(an approximation, not an exact moment) -- set provsql.rv_mc_samples = 0 "
      "to require an exact result instead", what.c_str(), n);
  return static_cast<unsigned>(n);
}

double mc_raw_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                     const std::string &what)
{
  auto samples = monteCarloScalarSamples(gc, g, mc_samples_or_throw(what));
  if (samples.empty()) return 0.0;
  // NaN samples come from sampling-undefined worlds, e.g. an
  // agg(SUM/AVG/MIN/MAX) over an empty group (SQL NULL).  Treat them
  // as missing observations of the moment rather than poisoning the
  // mean; only return NaN if every sample was undefined.
  double total = 0.0;
  std::size_t finite_count = 0;
  for (double x : samples) {
    if (std::isnan(x)) continue;
    total += std::pow(x, static_cast<double>(k));
    ++finite_count;
  }
  if (finite_count == 0) return std::numeric_limits<double>::quiet_NaN();
  return total / static_cast<double>(finite_count);
}

double mc_central_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                         double mu, const std::string &what)
{
  auto samples = monteCarloScalarSamples(gc, g, mc_samples_or_throw(what));
  if (samples.empty()) return 0.0;
  double total = 0.0;
  std::size_t finite_count = 0;
  for (double x : samples) {
    if (std::isnan(x)) continue;
    const double d = x - mu;
    total += std::pow(d, static_cast<double>(k));
    ++finite_count;
  }
  if (finite_count == 0) return std::numeric_limits<double>::quiet_NaN();
  return total / static_cast<double>(finite_count);
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
  if (cs.accepted.empty()) {
    /* 0-of-N accepted is the unmistakable signature of an infeasible
     * conditioning event: raising rv_mc_samples cannot help (the
     * acceptance probability is exactly 0).  Surface that directly
     * rather than the generic "raise samples or check satisfiability"
     * advice that applies to merely under-sampled events. */
    throw CircuitException(
      what + ": conditioning event is infeasible (0 of " +
      std::to_string(cs.attempted) +
      " Monte Carlo samples satisfied it)");
  }
  const unsigned floor = min_accepted_floor(cs.attempted);
  if (cs.accepted.size() < floor) {
    throw CircuitException(
      what + ": conditional MC accepted only " +
      std::to_string(cs.accepted.size()) + " out of " +
      std::to_string(cs.attempted) +
      " samples (need >= " + std::to_string(floor) +
      "); raise provsql.rv_mc_samples or tighten the event.");
  }
}

double mc_conditional_raw_moment(const GenericCircuit &gc, gate_t g,
                                 unsigned k, gate_t event_root,
                                 const std::string &what)
{
  auto cs = monteCarloConditionalScalarSamples(
              gc, g, event_root, mc_samples_or_throw(what));
  check_acceptance_or_throw(cs, what);
  // Mirror the unconditional path: NaN observations (sampling-
  // undefined worlds, typically empty-group SQL NULLs from
  // gate_agg) are excluded from the mean.
  double total = 0.0;
  std::size_t finite_count = 0;
  for (double x : cs.accepted) {
    if (std::isnan(x)) continue;
    total += std::pow(x, static_cast<double>(k));
    ++finite_count;
  }
  if (finite_count == 0) return std::numeric_limits<double>::quiet_NaN();
  return total / static_cast<double>(finite_count);
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
  std::size_t finite_count = 0;
  for (double x : cs.accepted) {
    if (std::isnan(x)) continue;
    const double d = x - mu;
    total += std::pow(d, static_cast<double>(k));
    ++finite_count;
  }
  if (finite_count == 0) return std::numeric_limits<double>::quiet_NaN();
  return total / static_cast<double>(finite_count);
}

double rec_expectation(const GenericCircuit &gc, gate_t g, FootprintCache &fp);
double rec_variance(const GenericCircuit &gc, gate_t g, FootprintCache &fp);
double rec_raw_moment(const GenericCircuit &gc, gate_t g, unsigned k,
                      FootprintCache &fp);

/* -----------------------------------------------------------------------
 * Likelihood-weighting (importance-sampling) posterior readouts.
 *
 * When the conditioning event is continuous-density evidence (it contains
 * a gate_observe), the analytic / rejection conditional paths do not apply:
 * draw latents from the prior and weight each draw by the observations'
 * densities, then report the weighted posterior statistic.  These helpers
 * turn one importance-sampling pass (WeightedPosterior) into a posterior
 * raw moment / central moment / quantile.
 * -------------------------------------------------------------------- */

/* Self-normalised weighted raw moment Σ w x^k / Σ w.  NaN particle values
 * (sampling-undefined worlds, e.g. an empty-group aggregate) are skipped,
 * mirroring the unconditional MC path. */
double weightedRawMoment(const WeightedPosterior &post, unsigned k)
{
  double sw = 0.0, swx = 0.0;
  for (const auto &[x, w] : post.particles) {
    if (std::isnan(x)) continue;
    sw  += w;
    swx += w * std::pow(x, static_cast<double>(k));
  }
  if (sw <= 0.0) return std::numeric_limits<double>::quiet_NaN();
  return swx / sw;
}

/* Self-normalised weighted central moment Σ w (x-mu)^k / Σ w. */
double weightedCentralMoment(const WeightedPosterior &post, unsigned k,
                             double mu)
{
  double sw = 0.0, swd = 0.0;
  for (const auto &[x, w] : post.particles) {
    if (std::isnan(x)) continue;
    sw  += w;
    swd += w * std::pow(x - mu, static_cast<double>(k));
  }
  if (sw <= 0.0) return std::numeric_limits<double>::quiet_NaN();
  return swd / sw;
}

/* Weighted empirical p-quantile: sort particles by value, walk the
 * cumulative weight, linearly interpolate at p·(Σw) (the percentile_cont
 * convention generalised to weights). */
double weightedQuantile(WeightedPosterior post, double p)
{
  auto &pts = post.particles;
  pts.erase(std::remove_if(pts.begin(), pts.end(),
                           [](const std::pair<double, double> &pw) {
                             return std::isnan(pw.first);
                           }),
            pts.end());
  if (pts.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(pts.begin(), pts.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  double total = 0.0;
  for (const auto &pw : pts) total += pw.second;
  if (!(total > 0.0)) return pts.front().first;
  const double target = p * total;
  double cum = 0.0;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const double prev = cum;
    cum += pts[i].second;
    if (cum >= target) {
      if (i == 0) return pts[0].first;
      /* Linear interpolation between the two straddling particles at their
       * cumulative-weight midpoints (matches the empirical MC quantile). */
      const double frac = (pts[i].second > 0.0)
                            ? (target - prev) / pts[i].second
                            : 0.0;
      return pts[i - 1].first + frac * (pts[i].first - pts[i - 1].first);
    }
  }
  return pts.back().first;
}

/* Guard a posterior pass: raise on infeasible (no positive-weight draw)
 * evidence, and warn when the effective sample size is degenerating. */
void checkPosteriorOrThrow(const WeightedPosterior &post,
                           const std::string &what)
{
  if (post.particles.empty() || post.weight_sum <= 0.0) {
    throw CircuitException(
      what + ": evidence is infeasible (no positive-weight draw among " +
      std::to_string(post.attempted) +
      " Monte Carlo samples); the observations may contradict the prior, "
      "or raise provsql.rv_mc_samples");
  }
  const double ess = post.effectiveSampleSize();
  const double nonzero = static_cast<double>(post.particles.size());
  if (provsql_ess_warn_fraction > 0.0 &&
      ess < provsql_ess_warn_fraction * nonzero) {
    provsql_warning(
      "%s: posterior effective sample size low (%.1f of %u accepted); "
      "likelihood weighting is degenerating -- raise provsql.rv_mc_samples, "
      "or the model has many observations per latent (defer to SMC)",
      what.c_str(), ess, static_cast<unsigned>(post.particles.size()));
  }
}

/**
 * @brief Try to evaluate @f$E[X^k \mid A]@f$ in closed form.
 *
 * Fires only when @p root is a bare @c gate_rv whose family has a
 * closed-form truncated moment (@c Distribution::truncatedRawMoment)
 * and the event walk under @p event_root collects a sound interval
 * constraint on it.
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
  const double lo = m->lo, hi = m->hi;

  /* Closed-form raw moment of the truncated distribution; constructed
   * once, then queried per moment order.  A family without a closed
   * form (Erlang: needs the regularised lower incomplete gamma) returns
   * nullopt and the caller falls through to MC. */
  const auto dist = makeDistribution(m->spec);
  auto raw = [&](unsigned q) -> std::optional<double> {
    if (q == 0) return 1.0;
    return dist->truncatedRawMoment(lo, hi, q);
  };

  if (!central) return raw(k);
  /* Central: E[(X - μ_A)^k | A] via the binomial expansion. */
  return centralFromRaw(k, raw);
}

/* A conditioning event of the shape @c "X op Y", where the target @c X and the
 * other operand @c Y are two independent bare @c gate_rv leaves. */
struct RvVsRvCond {
  DistributionSpec targetSpec;   /* X (the moment's target) */
  DistributionSpec otherSpec;    /* Y */
  bool targetGreater;            /* true for X > Y, false for X < Y */
};

/* Match @p event_root as a single @c gate_cmp comparing the target @p root
 * (a bare RV X) with an independent bare RV Y.  Returns std::nullopt for any
 * other shape (constant threshold -- handled by the truncation path;
 * conjunctions; agg comparisons; shared operand) so the caller falls through. */
std::optional<RvVsRvCond>
matchRvVsRvConditional(const GenericCircuit &gc, gate_t root, gate_t event_root)
{
  if (gc.getGateType(root) != gate_rv || gc.getGateType(event_root) != gate_cmp)
    return std::nullopt;
  auto specX = parse_distribution_spec(gc.getExtra(root));
  if (!specX) return std::nullopt;

  const auto &wires = gc.getWires(event_root);
  if (wires.size() != 2) return std::nullopt;
  bool ok = false;
  ComparisonOperator op = cmpOpFromOid(gc.getInfos(event_root).first, ok);
  if (!ok || op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
    return std::nullopt;

  gate_t other;
  bool targetLeft;
  if (wires[0] == root)      { other = wires[1]; targetLeft = true; }
  else if (wires[1] == root) { other = wires[0]; targetLeft = false; }
  else return std::nullopt;                 /* target not an operand */
  if (other == root || gc.getGateType(other) != gate_rv)
    return std::nullopt;                     /* X op X, or Y not a bare RV */
  auto specY = parse_distribution_spec(gc.getExtra(other));
  if (!specY) return std::nullopt;

  /* targetGreater: does the event assert X > Y?  If X is the left operand,
   * that is op in {GT,GE}; if X is the right operand (event Y op X), it is
   * op in {LT,LE}. */
  const bool greaterOp = (op == ComparisonOperator::GT ||
                          op == ComparisonOperator::GE);
  const bool lessOp = (op == ComparisonOperator::LT ||
                       op == ComparisonOperator::LE);
  bool targetGreater;
  if (targetLeft)  targetGreater = greaterOp;
  else             targetGreater = lessOp;
  return RvVsRvCond{*specX, *specY, targetGreater};
}

/* E[X^k | X op Y] for independent X, Y via a 1-D quadrature:
 *   E[X^k | X>Y] = (∫ x^k f_X(x) F_Y(x) dx) / (∫ f_X(x) F_Y(x) dx),
 * and the X<Y case swaps F_Y for 1-F_Y.  Composite Simpson over X's support;
 * exact for the Uniform-Uniform case (the integrands are low-degree
 * polynomials), high-accuracy otherwise.  Returns NaN if the event mass is
 * negligible or a density/CDF is undefined. */
double rvVsRvConditionalMoment(const DistributionSpec &X,
                               const DistributionSpec &Y,
                               bool targetGreater, unsigned k)
{
  // Construct both distributions once; the Simpson loop calls pdf/cdf on
  // them per point (never re-constructing per point).
  const auto dX = makeDistribution(X);
  const auto dY = makeDistribution(Y);
  double lo, hi;
  if (!dX->integrationRange(lo, hi))
    return std::numeric_limits<double>::quiet_NaN();

  auto base = [&](double x) {
    const double fX = dX->pdf(x);
    const double FY = dY->cdf(x);
    if (std::isnan(fX) || std::isnan(FY))
      return std::numeric_limits<double>::quiet_NaN();
    const double w = targetGreater ? FY : (1.0 - FY);  /* P(Y<x) / P(Y>x) */
    return fX * w;
  };
  const double den = simpsonIntegrate(lo, hi, kSimpsonPanels, base);
  if (std::isnan(den) || !(den > 1e-12))
    return std::numeric_limits<double>::quiet_NaN();
  const double num = simpsonIntegrate(lo, hi, kSimpsonPanels,
    [&](double x) {
      return std::pow(x, static_cast<double>(k)) * base(x);
    });
  if (std::isnan(num))
    return std::numeric_limits<double>::quiet_NaN();
  return num / den;
}

/* Closed-form (quadrature) E[X^k | X op Y] / central moment for an
 * RV-vs-RV conditioning event.  Mirrors @c try_truncated_closed_form for the
 * constant-threshold case. */
std::optional<double>
try_rvVsRv_conditional_moment(const GenericCircuit &gc, gate_t root,
                              gate_t event_root, unsigned k, bool central)
{
  auto m = matchRvVsRvConditional(gc, root, event_root);
  if (!m) return std::nullopt;

  auto raw = [&](unsigned q) -> std::optional<double> {
    if (q == 0) return 1.0;
    double r = rvVsRvConditionalMoment(m->targetSpec, m->otherSpec,
                                       m->targetGreater, q);
    if (std::isnan(r)) return std::nullopt;
    return r;
  };

  if (!central) return raw(k);
  return centralFromRaw(k, raw);
}

/* One comparison of a pivot RV X against another operand, a factor in the
 * pivot-conjunction integrand.  `other` is an independent bare RV (its CDF
 * weights the integrand) or, when `isConst`, a constant threshold that clips
 * the integration window.  `pivotGreater` is true when the factor asserts
 * X > operand. */
struct PivotFactor {
  bool isConst;
  DistributionSpec other;   /* valid iff !isConst */
  double konst;             /* valid iff isConst */
  bool pivotGreater;
};

/* ∫ x^k f_X(x) Π_j W_j(x) dx over X's support, where each RV factor contributes
 * W_j(x) = F_{Y_j}(x)  for X>Y_j  and  1-F_{Y_j}(x)  for X<Y_j, and each
 * constant factor clips the window to {x : x>c} / {x : x<c}.  With k=0 this is
 * the joint probability P(∧_j comparisons).  Because the comparisons all share
 * the single pivot X and the other operands are independent, marginalising each
 * Y_j analytically collapses the joint to this 1-D integral.  Composite Simpson
 * (exact for the polynomial integrands of the Uniform case).  Returns NaN if a
 * density / CDF is undefined. */
double pivotConjunctionIntegral(const DistributionSpec &X,
                                const std::vector<PivotFactor> &factors,
                                unsigned k)
{
  const auto dX = makeDistribution(X);
  double lo, hi;
  if (!dX->integrationRange(lo, hi))
    return std::numeric_limits<double>::quiet_NaN();
  for (const auto &f : factors)
    if (f.isConst) {
      if (f.pivotGreater) lo = std::max(lo, f.konst);
      else                hi = std::min(hi, f.konst);
    }
  if (!(hi > lo)) return 0.0;

  std::vector<std::unique_ptr<Distribution>> others;
  std::vector<bool> greater;
  for (const auto &f : factors)
    if (!f.isConst) {
      others.push_back(makeDistribution(f.other));
      greater.push_back(f.pivotGreater);
    }

  return simpsonIntegrate(lo, hi, kSimpsonPanels, [&](double x) {
    const double fX = dX->pdf(x);
    if (std::isnan(fX)) return std::numeric_limits<double>::quiet_NaN();
    double w = fX;
    for (std::size_t j = 0; j < others.size(); ++j) {
      const double FY = others[j]->cdf(x);
      if (std::isnan(FY)) return std::numeric_limits<double>::quiet_NaN();
      w *= greater[j] ? FY : (1.0 - FY);
    }
    return std::pow(x, static_cast<double>(k)) * w;
  });
}

/* A conditioning event that is a conjunction of comparisons all sharing the
 * target bare RV X, each against an independent bare RV or a constant. */
struct PivotConjunctionCond {
  DistributionSpec targetSpec;
  std::vector<PivotFactor> factors;
};

/* Match @p event_root as a @c gate_times (AND) of >=2 @c gate_cmp, each
 * comparing the target @p root (bare RV X) with an independent bare RV or a
 * constant.  Returns nullopt for a single comparison (the truncation / rvVsRv
 * paths own that), an agg comparison, a cmp not involving X, a non-bare other
 * operand, or a repeated / shared other operand (would break independence). */
std::optional<PivotConjunctionCond>
matchPivotConjunctionConditional(const GenericCircuit &gc, gate_t root,
                                 gate_t event_root)
{
  if (gc.getGateType(root) != gate_rv ||
      gc.getGateType(event_root) != gate_times)
    return std::nullopt;
  auto specX = parse_distribution_spec(gc.getExtra(root));
  if (!specX) return std::nullopt;

  const auto &kids = gc.getWires(event_root);
  if (kids.size() < 2) return std::nullopt;
  std::vector<PivotFactor> factors;
  std::set<gate_t> othersSeen;
  for (gate_t c : kids) {
    if (gc.getGateType(c) != gate_cmp) return std::nullopt;
    const auto &w = gc.getWires(c);
    if (w.size() != 2) return std::nullopt;
    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(c).first, ok);
    if (!ok || op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
      return std::nullopt;
    gate_t other; bool targetLeft;
    if (w[0] == root)      { other = w[1]; targetLeft = true; }
    else if (w[1] == root) { other = w[0]; targetLeft = false; }
    else return std::nullopt;
    const bool greaterOp = (op == ComparisonOperator::GT ||
                            op == ComparisonOperator::GE);
    const bool lessOp    = (op == ComparisonOperator::LT ||
                            op == ComparisonOperator::LE);
    const bool pivotGreater = targetLeft ? greaterOp : lessOp;

    PivotFactor f;
    if (gc.getGateType(other) == gate_value) {
      f = {true, DistributionSpec{}, parseDoubleStrict(gc.getExtra(other)),
           pivotGreater};
    } else if (gc.getGateType(other) == gate_rv && other != root) {
      if (!othersSeen.insert(other).second) return std::nullopt;
      auto specY = parse_distribution_spec(gc.getExtra(other));
      if (!specY) return std::nullopt;
      f = {false, *specY, 0.0, pivotGreater};
    } else return std::nullopt;
    factors.push_back(std::move(f));
  }
  return PivotConjunctionCond{*specX, std::move(factors)};
}

/* Closed-form (quadrature) E[X^k | ∧_j (X op Y_j)] for a conjunction of
 * comparisons sharing the target X.  E[X^k | A] = I_k / I_0 with
 * I_q = ∫ x^q f_X Π_j W_j dx; central via the usual binomial expansion. */
std::optional<double>
try_pivotConjunction_conditional_moment(const GenericCircuit &gc, gate_t root,
                                        gate_t event_root, unsigned k,
                                        bool central)
{
  auto m = matchPivotConjunctionConditional(gc, root, event_root);
  if (!m) return std::nullopt;

  const double den = pivotConjunctionIntegral(m->targetSpec, m->factors, 0);
  if (std::isnan(den) || !(den > 1e-12))
    return std::nullopt;   /* negligible / undefined event -> infeasible or MC */
  auto raw = [&](unsigned q) -> std::optional<double> {
    if (q == 0) return 1.0;
    const double num = pivotConjunctionIntegral(m->targetSpec, m->factors, q);
    if (std::isnan(num)) return std::nullopt;
    return num / den;
  };

  if (!central) return raw(k);
  return centralFromRaw(k, raw);
}

/* Closed-form image of a unary LN / EXP transform over a bare gate_rv
 * child, via the TransformRuleRegistry (exp(normal) is lognormal,
 * ln(lognormal) is normal).  Read-only: unlike the hybrid simplifier's
 * fold of the same shape, nothing is rewritten, so no shared-RV
 * identity can be decoupled -- which is why the moment path may use it
 * even though it deliberately does not run the simplifier.  nullptr
 * when the child is not a bare parseable rv or no rule covers the
 * family; the caller falls to MC. */
std::unique_ptr<Distribution> transform_image(const GenericCircuit &gc,
                                              gate_t g,
                                              provsql_arith_op op)
{
  const char *transform = op == PROVSQL_ARITH_LN ? "ln"
                        : op == PROVSQL_ARITH_EXP ? "exp"
                        : nullptr;
  if (!transform) return nullptr;
  const auto &wires = gc.getWires(g);
  if (wires.size() != 1 || gc.getGateType(wires[0]) != gate_rv)
    return nullptr;
  auto spec = parse_distribution_spec(gc.getExtra(wires[0]));
  if (!spec) return nullptr;
  return closeTransform(transform, *makeDistribution(*spec));
}

/* Closed-form distribution of a TIMES product over bare, pairwise
 * distinct gate_rv factors (plus gate_value scalars), via the
 * ProductRuleRegistry -- read-only, like transform_image, so no shared
 * identity is disturbed.  Quantiles need it (they do not factor the way
 * the disjoint-product moment shortcuts do); nullptr when the shape or
 * family combination is outside the registered closures. */
std::unique_ptr<Distribution> product_image(const GenericCircuit &gc,
                                            gate_t g)
{
  const auto &wires = gc.getWires(g);
  if (wires.empty()) return nullptr;
  double c_total = 1.0;
  std::vector<std::unique_ptr<Distribution>> dists;
  std::vector<const Distribution *> factors;
  std::set<gate_t> seen;
  for (gate_t w : wires) {
    const auto t = gc.getGateType(w);
    if (t == gate_value) {
      try { c_total *= parseDoubleStrict(gc.getExtra(w)); }
      catch (const CircuitException &) { return nullptr; }
      continue;
    }
    if (t != gate_rv) return nullptr;
    if (!seen.insert(w).second) return nullptr;   /* dependent */
    auto spec = parse_distribution_spec(gc.getExtra(w));
    if (!spec) return nullptr;
    dists.push_back(makeDistribution(*spec));
    factors.push_back(dists.back().get());
  }
  if (factors.empty()) return nullptr;
  /* A single RV with scalar factors is just the affine image; two or
   * more dispatch through the product registry. */
  std::unique_ptr<Distribution> combined;
  if (factors.size() == 1)
    combined = std::move(dists.front());
  else
    combined = closeProductFactors(factors);
  if (!combined) return nullptr;
  if (c_total != 1.0) return combined->scale(c_total);
  return combined;
}

/* ------------------------------------------------------------------------
 * Tier A of the gate_case guard-partition integrator: a CASE that is a
 * piecewise function of a SINGLE pivot random variable X.  Every guard is a
 * bare gate_cmp comparing X against a constant, and every arm value (and the
 * default) is affine in X -- a constant, or a*X + b.  This is exactly the
 * abs / ReLU / clamp piecewise-sugar shape.
 *
 * Since the whole CASE depends on one RV, E[case^k] is a 1-D integral that
 * partitions X's support at the guard thresholds: on each sub-interval a
 * single arm fires (first-match, reproducing MonteCarloSampler's order), and
 * its affine value's k-th moment over that interval is a binomial combination
 * of the truncated raw moments
 *   ∫_lo^hi x^j f(x) dx = truncatedRawMoment(lo,hi,j) · (F(hi)-F(lo)).
 * Returns std::nullopt when the shape is not single-pivot-affine or the family
 * lacks a closed-form truncated moment, so the caller falls back to MC. */

struct AffineArm { double a, b; };   /* value = a*X + b */

std::optional<AffineArm>
affineInPivot(const GenericCircuit &gc, gate_t arm, gate_t pivot)
{
  const auto t = gc.getGateType(arm);
  if (t == gate_value)
    return AffineArm{0.0, parseDoubleStrict(gc.getExtra(arm))};
  if (t == gate_rv)
    return (arm == pivot) ? std::optional<AffineArm>(AffineArm{1.0, 0.0})
                          : std::nullopt;
  if (t == gate_arith) {
    const auto op = static_cast<provsql_arith_op>(gc.getInfos(arm).first);
    const auto &w = gc.getWires(arm);
    auto isPivot = [&](gate_t x) {
      return gc.getGateType(x) == gate_rv && x == pivot;
    };
    auto constVal = [&](gate_t x, double &out) {
      if (gc.getGateType(x) != gate_value) return false;
      out = parseDoubleStrict(gc.getExtra(x));
      return true;
    };
    double c;
    if (op == PROVSQL_ARITH_NEG && w.size() == 1 && isPivot(w[0]))
      return AffineArm{-1.0, 0.0};
    if (op == PROVSQL_ARITH_PLUS && w.size() == 2) {
      if (isPivot(w[0]) && constVal(w[1], c)) return AffineArm{1.0, c};
      if (isPivot(w[1]) && constVal(w[0], c)) return AffineArm{1.0, c};
    }
    if (op == PROVSQL_ARITH_MINUS && w.size() == 2) {
      if (isPivot(w[0]) && constVal(w[1], c)) return AffineArm{1.0, -c};   /* X - c */
      if (isPivot(w[1]) && constVal(w[0], c)) return AffineArm{-1.0, c};   /* c - X */
    }
    if (op == PROVSQL_ARITH_TIMES && w.size() == 2) {
      if (isPivot(w[0]) && constVal(w[1], c)) return AffineArm{c, 0.0};
      if (isPivot(w[1]) && constVal(w[0], c)) return AffineArm{c, 0.0};
    }
  }
  return std::nullopt;
}

std::optional<double>
singlePivotCaseRawMoment(const GenericCircuit &gc, gate_t g, unsigned k)
{
  if (gc.getGateType(g) != gate_case) return std::nullopt;
  const auto &wires = gc.getWires(g);
  if (wires.size() < 3 || wires.size() % 2 == 0) return std::nullopt;
  const std::size_t m = wires.size() / 2;   /* guard/value pairs */

  /* ---- identify the common pivot RV and each guard's constant threshold ---- */
  gate_t pivot{}; bool havePivot = false;
  struct Guard { double c; ComparisonOperator op; bool pivotLeft; };
  std::vector<Guard> guards;
  guards.reserve(m);
  for (std::size_t i = 0; i < m; ++i) {
    gate_t gd = wires[2 * i];
    if (gc.getGateType(gd) != gate_cmp) return std::nullopt;
    const auto &gw = gc.getWires(gd);
    if (gw.size() != 2) return std::nullopt;
    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(gd).first, ok);
    if (!ok || op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
      return std::nullopt;
    gate_t rvSide, constSide; bool pivotLeft;
    if (gc.getGateType(gw[0]) == gate_rv && gc.getGateType(gw[1]) == gate_value) {
      rvSide = gw[0]; constSide = gw[1]; pivotLeft = true;
    } else if (gc.getGateType(gw[1]) == gate_rv &&
               gc.getGateType(gw[0]) == gate_value) {
      rvSide = gw[1]; constSide = gw[0]; pivotLeft = false;
    } else return std::nullopt;
    if (!havePivot) { pivot = rvSide; havePivot = true; }
    else if (rvSide != pivot) return std::nullopt;  /* multi-pivot -> Tier B */
    guards.push_back({parseDoubleStrict(gc.getExtra(constSide)), op, pivotLeft});
  }
  if (!havePivot) return std::nullopt;

  auto spec = parse_distribution_spec(gc.getExtra(pivot));
  if (!spec) return std::nullopt;
  const auto dist = makeDistribution(*spec);

  /* ---- classify each arm (values then default) as affine in the pivot ---- */
  std::vector<AffineArm> arms;   /* arms[0..m-1] value branches, arms[m] default */
  arms.reserve(m + 1);
  for (std::size_t i = 0; i < m; ++i) {
    auto af = affineInPivot(gc, wires[2 * i + 1], pivot);
    if (!af) return std::nullopt;
    arms.push_back(*af);
  }
  {
    auto af = affineInPivot(gc, wires.back(), pivot);
    if (!af) return std::nullopt;
    arms.push_back(*af);
  }

  /* First-match arm selection at a pivot value x (guards are half-lines, so
   * this is constant across the open interior of every partition segment). */
  auto guardTrue = [](const Guard &gd, double x) -> bool {
    const double lhs = gd.pivotLeft ? x    : gd.c;
    const double rhs = gd.pivotLeft ? gd.c : x;
    switch (gd.op) {
      case ComparisonOperator::LT: return lhs <  rhs;
      case ComparisonOperator::LE: return lhs <= rhs;
      case ComparisonOperator::GT: return lhs >  rhs;
      case ComparisonOperator::GE: return lhs >= rhs;
      default:                     return false;
    }
  };
  auto pickArm = [&](double x) -> const AffineArm & {
    for (std::size_t i = 0; i < m; ++i)
      if (guardTrue(guards[i], x)) return arms[i];
    return arms[m];
  };

  /* ---- partition the pivot's support at the (sorted, deduped) thresholds ---- */
  std::vector<double> cuts;
  for (const auto &gd : guards) cuts.push_back(gd.c);
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

  const double NINF = -std::numeric_limits<double>::infinity();
  const double PINF =  std::numeric_limits<double>::infinity();

  double total = 0.0;
  const std::size_t nseg = cuts.size() + 1;
  for (std::size_t s = 0; s < nseg; ++s) {
    const double lo = (s == 0)           ? NINF : cuts[s - 1];
    const double hi = (s == cuts.size()) ? PINF : cuts[s];
    if (lo == hi) continue;

    double t;                            /* interior test point of (lo, hi) */
    if (lo == NINF && hi == PINF) t = 0.0;
    else if (lo == NINF)          t = hi - 1.0;
    else if (hi == PINF)          t = lo + 1.0;
    else                          t = 0.5 * (lo + hi);
    const AffineArm &arm = pickArm(t);

    const double dF = dist->cdf(hi) - dist->cdf(lo);
    if (std::isnan(dF)) return std::nullopt;
    if (dF <= 0.0) continue;

    /* E[(aX+b)^k · 1(lo<X<hi)] = Σ_j C(k,j) a^j b^{k-j} ∫ x^j f dx. */
    for (unsigned j = 0; j <= k; ++j) {
      const double aj = std::pow(arm.a, static_cast<double>(j));
      if (aj == 0.0) continue;          /* constant arm: only the j=0 term */
      const double coef = binomial(k, j) * aj
                        * std::pow(arm.b, static_cast<double>(k - j));
      if (coef == 0.0) continue;
      double intj;
      if (j == 0) {
        intj = dF;
      } else {
        auto trm = dist->truncatedRawMoment(lo, hi, j);
        if (!trm) return std::nullopt;  /* family lacks the closed form -> MC */
        intj = *trm * dF;
      }
      total += coef * intj;
    }
  }
  return total;
}

/* Tier B of the guard-partition integrator: a two-arm CASE
 *   CASE WHEN (a op b) THEN A ELSE B
 * whose single guard compares two DISTINCT independent bare RVs a, b, and whose
 * arms A (under the guard) and B (under its complement) are each affine in one
 * of a, b.  Each arm's contribution is a 1-D integral over its value's pivot
 * RV, the other operand marginalised to a CDF weight -- exactly the pivot-
 * conjunction integral with a single factor.  nullopt for any other shape
 * (guard vs a constant is Tier A; >2 arms; an arm not affine in a comparison
 * operand; a family without a usable pdf/cdf), so the caller falls to MC. */
std::optional<double>
twoArmCaseRawMoment(const GenericCircuit &gc, gate_t g, unsigned k)
{
  if (gc.getGateType(g) != gate_case) return std::nullopt;
  const auto &wires = gc.getWires(g);
  if (wires.size() != 3) return std::nullopt;   /* one guard/value + default */
  gate_t guard = wires[0];
  if (gc.getGateType(guard) != gate_cmp) return std::nullopt;
  const auto &gw = gc.getWires(guard);
  if (gw.size() != 2) return std::nullopt;
  bool ok = false;
  ComparisonOperator op = cmpOpFromOid(gc.getInfos(guard).first, ok);
  if (!ok || op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
    return std::nullopt;
  gate_t a = gw[0], b = gw[1];
  if (gc.getGateType(a) != gate_rv || gc.getGateType(b) != gate_rv || a == b)
    return std::nullopt;
  auto specA = parse_distribution_spec(gc.getExtra(a));
  auto specB = parse_distribution_spec(gc.getExtra(b));
  if (!specA || !specB) return std::nullopt;
  /* op GT/GE with a as the left operand => the guard asserts a > b. */
  const bool guard_a_gt_b = (op == ComparisonOperator::GT ||
                             op == ComparisonOperator::GE);

  /* E[value^k · 1(region)] for an arm whose value is affine in a or b, where
   * `regionAgtB` says the arm's region is (a > b). */
  auto armContribution = [&](gate_t armGate, bool regionAgtB)
                          -> std::optional<double> {
    for (int which = 0; which < 2; ++which) {
      gate_t pv = (which == 0) ? a : b;
      auto af = affineInPivot(gc, armGate, pv);
      if (!af) continue;
      const DistributionSpec &pvSpec = (which == 0) ? *specA : *specB;
      const DistributionSpec &otSpec = (which == 0) ? *specB : *specA;
      /* Weight orientation: with pivot a the region a>b integrates b to F_b(a);
       * with pivot b the same region means b<a, i.e. 1-F_a(b). */
      const bool pivotGreater = (which == 0) ? regionAgtB : !regionAgtB;
      const PivotFactor f{false, otSpec, 0.0, pivotGreater};
      double total = 0.0;
      for (unsigned j = 0; j <= k; ++j) {
        const double aj = std::pow(af->a, static_cast<double>(j));
        if (aj == 0.0) continue;
        const double coef = binomial(k, j) * aj
                          * std::pow(af->b, static_cast<double>(k - j));
        if (coef == 0.0) continue;
        const double I = pivotConjunctionIntegral(pvSpec, {f}, j);
        if (std::isnan(I)) return std::nullopt;
        total += coef * I;
      }
      return total;
    }
    return std::nullopt;   /* arm not affine in either comparison operand */
  };

  auto c0 = armContribution(wires[1], guard_a_gt_b);    /* value under guard */
  if (!c0) return std::nullopt;
  auto c1 = armContribution(wires[2], !guard_a_gt_b);   /* default under ¬guard */
  if (!c1) return std::nullopt;
  return *c0 + *c1;
}

/* Evaluate a gate_case guard (a bare gate_cmp, or an AND/OR tree of them over
 * value RVs) under a strict ordering `rank` of the RVs (higher rank = larger
 * value).  Returns nullopt if the guard references a constant, an RV outside
 * `rank`, or an unsupported gate -- i.e. the CASE is not a pure RV tournament. */
std::optional<bool>
evalGuardUnderOrder(const GenericCircuit &gc, gate_t guard,
                    const std::unordered_map<gate_t, int> &rank)
{
  const auto t = gc.getGateType(guard);
  if (t == gate_cmp) {
    const auto &w = gc.getWires(guard);
    if (w.size() != 2) return std::nullopt;
    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(guard).first, ok);
    if (!ok) return std::nullopt;
    auto ia = rank.find(w[0]), ib = rank.find(w[1]);
    if (ia == rank.end() || ib == rank.end()) return std::nullopt;
    const int ra = ia->second, rb = ib->second;   /* distinct RVs -> ra != rb */
    switch (op) {
      case ComparisonOperator::LT:
      case ComparisonOperator::LE: return ra < rb;
      case ComparisonOperator::GT:
      case ComparisonOperator::GE: return ra > rb;
      case ComparisonOperator::EQ: return false;   /* a.s. for continuous RVs */
      case ComparisonOperator::NE: return true;
    }
    return std::nullopt;
  }
  if (t == gate_times || t == gate_plus) {
    const bool isAnd = (t == gate_times);
    bool acc = isAnd;
    for (gate_t c : gc.getWires(guard)) {
      auto v = evalGuardUnderOrder(gc, c, rank);
      if (!v) return std::nullopt;
      acc = isAnd ? (acc && *v) : (acc || *v);
    }
    return acc;
  }
  return std::nullopt;
}

/* Tier C: a first-match gate_case that computes the max or min of a set of
 * independent bare RVs.  Recognised by simulating the first-match selection
 * over every strict ordering of the value RVs (continuous RVs tie with
 * probability 0, so strict orders capture the a.s. behaviour): if the selected
 * value is always the maximum (resp. minimum), the CASE is that order
 * statistic, whose k-th moment is
 *   Σ_i ∫ x^k f_{X_i}(x) Π_{j≠i} F_{X_j}(x) dx      (max; min flips F to 1-F),
 * a sum of pivot-conjunction integrals.  Guards may be AND/OR trees of RV-vs-RV
 * comparisons among the value RVs; a constant or an outside RV in a guard, a
 * non-bare-RV arm, or too many RVs (the n! simulation is capped) decline. */
std::optional<double>
orderStatCaseRawMoment(const GenericCircuit &gc, gate_t g, unsigned k)
{
  if (gc.getGateType(g) != gate_case) return std::nullopt;
  const auto &wires = gc.getWires(g);
  if (wires.size() < 3 || wires.size() % 2 == 0) return std::nullopt;
  const std::size_t m = wires.size() / 2;

  /* Arms (values + default) must all be bare RVs; collect the distinct set. */
  std::vector<gate_t> armRV(m + 1);
  std::vector<gate_t> uniq;
  std::unordered_map<gate_t, DistributionSpec> specOf;
  auto noteRV = [&](gate_t v) -> bool {
    if (gc.getGateType(v) != gate_rv) return false;
    if (specOf.find(v) == specOf.end()) {
      auto sp = parse_distribution_spec(gc.getExtra(v));
      if (!sp) return false;
      specOf.emplace(v, *sp);
      uniq.push_back(v);
    }
    return true;
  };
  for (std::size_t i = 0; i < m; ++i) {
    armRV[i] = wires[2 * i + 1];
    if (!noteRV(armRV[i])) return std::nullopt;
  }
  armRV[m] = wires.back();
  if (!noteRV(armRV[m])) return std::nullopt;

  const std::size_t n = uniq.size();
  if (n < 2 || n > 7) return std::nullopt;   /* n! simulation cap */

  /* Guards must reference only the value RVs (checked implicitly by
   * evalGuardUnderOrder, which declines on any leaf outside `rank`). */
  std::vector<gate_t> guards(m);
  for (std::size_t i = 0; i < m; ++i) guards[i] = wires[2 * i];

  /* Simulate first-match over every strict ordering of the RVs. */
  std::vector<std::size_t> perm(n);
  for (std::size_t i = 0; i < n; ++i) perm[i] = i;
  bool alwaysMax = true, alwaysMin = true;
  do {
    std::unordered_map<gate_t, int> rank;
    for (std::size_t i = 0; i < n; ++i) rank[uniq[perm[i]]] = static_cast<int>(i);
    /* max / min RV under this ordering (highest / lowest rank). */
    gate_t maxRV = uniq[perm[n - 1]], minRV = uniq[perm[0]];

    gate_t selected = armRV[m];   /* default if no guard fires */
    for (std::size_t i = 0; i < m; ++i) {
      auto gv = evalGuardUnderOrder(gc, guards[i], rank);
      if (!gv) return std::nullopt;
      if (*gv) { selected = armRV[i]; break; }
    }
    if (selected != maxRV) alwaysMax = false;
    if (selected != minRV) alwaysMin = false;
    if (!alwaysMax && !alwaysMin) return std::nullopt;
  } while (std::next_permutation(perm.begin(), perm.end()));

  const bool isMax = alwaysMax;   /* prefer max if (degenerately) both hold */

  /* Σ_i ∫ x^k f_{X_i} Π_{j≠i} (F_{X_j} or 1-F_{X_j}) dx. */
  double total = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    std::vector<PivotFactor> factors;
    factors.reserve(n - 1);
    for (std::size_t j = 0; j < n; ++j) {
      if (j == i) continue;
      factors.push_back({false, specOf.at(uniq[j]), 0.0, isMax});
    }
    const double I = pivotConjunctionIntegral(specOf.at(uniq[i]), factors, k);
    if (std::isnan(I)) return std::nullopt;
    total += I;
  }
  return total;
}

/* Analytic k-th raw moment of a gate_case, trying each guard-partition tier. */
std::optional<double>
caseAnalyticRawMoment(const GenericCircuit &gc, gate_t g, unsigned k)
{
  if (auto v = singlePivotCaseRawMoment(gc, g, k)) return v;
  if (auto v = twoArmCaseRawMoment(gc, g, k)) return v;
  if (auto v = orderStatCaseRawMoment(gc, g, k)) return v;
  return std::nullopt;
}

double rec_expectation(const GenericCircuit &gc, gate_t g, FootprintCache &fp)
{
  const auto type = gc.getGateType(g);
  switch (type) {
    case gate_value:
      return parseDoubleStrict(gc.getExtra(g));
    case gate_rv: {
      // A latent (parametric) leaf -- a parameter is itself a random
      // variable -- has no constant-parameter closed form.  But the MEAN
      // still decomposes exactly when the family's mean is affine in its
      // parameters (Normal mean = μ, Uniform mean = (a+b)/2, inverse-
      // Gaussian mean = μ): E[X] = E[mean(θ)] = mean(E[θ]) by linearity of
      // expectation (no independence assumption), so recurse into the
      // parameter wires -- no MC.  Nonlinear means (Exponential 1/λ,
      // Gamma k/λ, ...) keep meanIsAffine() = false and fall through to MC.
      if (rvIsParametric(gc, g)) {
        auto tmpl = parse_distribution_template(gc.getExtra(g));
        if (tmpl && familyMeanIsAffine(*tmpl)) {
          const auto &w = gc.getWires(g);
          auto param_mean = [&](const DistributionParam &p) {
            return p.wire_slot < 0 ? p.literal
                                   : rec_expectation(gc, w[p.wire_slot], fp);
          };
          return tmpl->family
                   ->factory(param_mean(tmpl->p1), param_mean(tmpl->p2))
                   ->mean();
        }
        return mc_raw_moment(gc, g, 1, "Expectation of a latent gate_rv");
      }
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
        case PROVSQL_ARITH_MAX:
        case PROVSQL_ARITH_MIN: {
          // Order statistics have no linearity to push through.  Exact
          // closed form for i.i.d. Uniform / Exponential; the layer-cake
          // 1-D quadrature for any other independent bare-RV mix (mixed
          // families, non-identical parameters, Normal); Monte Carlo only
          // when leaves are shared / correlated.
          const bool isMax = (op == PROVSQL_ARITH_MAX);
          if (auto v = iidOrderStatMean(gc, g, isMax, fp))
            return *v;
          if (auto v = mixedOrderStatMean(gc, g, isMax, fp))
            return *v;
          return mc_raw_moment(gc, g, 1,
            "Expectation of gate_arith " + std::string(isMax ? "MAX" : "MIN"));
        }
        case PROVSQL_ARITH_POW:
        case PROVSQL_ARITH_LN:
        case PROVSQL_ARITH_EXP:
          // Nonlinear transforms: expectation does not commute with the
          // map, so there is no linearity to push through.  A registered
          // closed-form image (exp(normal) is lognormal, ln(lognormal)
          // is normal) gives the exact answer; otherwise the empirical
          // estimate is the general one.
          if (auto image = transform_image(gc, g, op))
            return image->mean();
          return mc_raw_moment(gc, g, 1,
            "Expectation of a gate_arith nonlinear transform");
        case PROVSQL_ARITH_PERCENTILE:
          // Order-statistic aggregate over a random member set: no closed
          // form; the sampler sorts and interpolates each draw.
          return mc_raw_moment(gc, g, 1,
            "Expectation of a gate_arith PERCENTILE");
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
    case gate_case:
      if (auto v = caseAnalyticRawMoment(gc, g, 1))
        return *v;
      return mc_raw_moment(gc, g, 1, "Expectation of gate type gate_case");
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
      // Latent (parametric) leaf: no constant-parameter closed form.
      // Var(normal(M,1)) = 1 + Var(M) etc. is exact in expectation under
      // MC (the sampler draws the latent then the leaf per iteration).
      if (rvIsParametric(gc, g)) {
        const std::string what = "Variance of a latent gate_rv";
        const double mu = mc_raw_moment(gc, g, 1, what);
        return mc_central_moment(gc, g, 2, mu, what);
      }
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
        case PROVSQL_ARITH_MAX:
        case PROVSQL_ARITH_MIN:
          // No closed-form variance decomposition for order statistics; MC.
          return mc_var(
            "Variance of gate_arith " +
            std::string(op == PROVSQL_ARITH_MAX ? "MAX" : "MIN"));
        case PROVSQL_ARITH_POW:
        case PROVSQL_ARITH_LN:
        case PROVSQL_ARITH_EXP:
          if (auto image = transform_image(gc, g, op))
            return image->variance();
          return mc_var("Variance of a gate_arith nonlinear transform");
        case PROVSQL_ARITH_PERCENTILE:
          return mc_var("Variance of a gate_arith PERCENTILE");
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
    case gate_case: {
      if (auto m2 = caseAnalyticRawMoment(gc, g, 2))
        if (auto m1 = caseAnalyticRawMoment(gc, g, 1))
          return *m2 - (*m1) * (*m1);
      const std::string what = "Variance of gate type gate_case";
      const double mu = mc_raw_moment(gc, g, 1, what);
      return mc_central_moment(gc, g, 2, mu, what);
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
      // Latent (parametric) leaf: no constant-parameter closed form.
      if (rvIsParametric(gc, g))
        return mc_raw_moment(gc, g, k, "Raw moment of a latent gate_rv");
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
        case PROVSQL_ARITH_MAX:
        case PROVSQL_ARITH_MIN:
          // Order-statistic raw moments have no elementary decomposition; MC.
          return mc_raw_moment(gc, g, k,
            "Raw moment of gate_arith " +
            std::string(op == PROVSQL_ARITH_MAX ? "MAX" : "MIN"));
        case PROVSQL_ARITH_POW:
        case PROVSQL_ARITH_LN:
        case PROVSQL_ARITH_EXP:
          if (auto image = transform_image(gc, g, op))
            return image->rawMoment(k);
          return mc_raw_moment(gc, g, k,
            "Raw moment of a gate_arith nonlinear transform");
        case PROVSQL_ARITH_PERCENTILE:
          return mc_raw_moment(gc, g, k,
            "Raw moment of a gate_arith PERCENTILE");
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
    case gate_case:
      if (auto v = caseAnalyticRawMoment(gc, g, k))
        return *v;
      return mc_raw_moment(gc, g, k, "Raw moment of gate type gate_case");
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

[[noreturn]] void raise_infeasible_event(const GenericCircuit &gc, gate_t root)
{
  (void)gc; (void)root;
  throw CircuitException(
    "conditioning event is infeasible (empty intersection with the "
    "random variable's support)");
}

double conditional_raw_moment(const GenericCircuit &gc, gate_t root,
                              unsigned k, gate_t event_root)
{
  if (k == 0) return 1.0;
  /* Continuous-density evidence (latent-variable posterior): likelihood
   * weighting.  The closed-form / rejection paths below assume a bare-rv
   * truncation event, so they do not apply. */
  if (circuitHasObserve(gc, event_root)) {
    const std::string what = "Posterior raw moment";
    auto post = importanceSampleConditional(
                  gc, root, event_root, mc_samples_or_throw(what));
    checkPosteriorOrThrow(post, what);
    return weightedRawMoment(post, k);
  }
  if (auto cf = try_truncated_closed_form(gc, root, event_root, k, false))
    return *cf;
  if (auto cf = try_rvVsRv_conditional_moment(gc, root, event_root, k, false))
    return *cf;
  if (auto cf = try_pivotConjunction_conditional_moment(gc, root, event_root,
                                                        k, false))
    return *cf;
  if (eventIsProvablyInfeasible(gc, root, event_root))
    raise_infeasible_event(gc, root);
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
  /* Continuous-density evidence: one importance-sampling pass yields both
   * the posterior mean and the central moment (no resampling). */
  if (circuitHasObserve(gc, event_root)) {
    const std::string what = "Posterior central moment";
    auto post = importanceSampleConditional(
                  gc, root, event_root, mc_samples_or_throw(what));
    checkPosteriorOrThrow(post, what);
    const double mu = weightedRawMoment(post, 1);
    return weightedCentralMoment(post, k, mu);
  }
  if (auto cf = try_truncated_closed_form(gc, root, event_root, k, true))
    return *cf;
  if (auto cf = try_rvVsRv_conditional_moment(gc, root, event_root, k, true))
    return *cf;
  if (auto cf = try_pivotConjunction_conditional_moment(gc, root, event_root,
                                                        k, true))
    return *cf;
  if (eventIsProvablyInfeasible(gc, root, event_root))
    raise_infeasible_event(gc, root);
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

/* ─────────────────────── quantiles (§B.1) ─────────────────────── */

namespace {

/* Empirical p-quantile with the linear-interpolation convention
 * PostgreSQL's percentile_cont uses (type 7: h = p·(n-1)).  NaN
 * observations (sampling-undefined worlds, e.g. empty-group SQL NULLs
 * from gate_agg) are dropped like the MC moment estimators do; NaN if
 * every sample was undefined. */
double empirical_quantile(std::vector<double> xs, double p)
{
  xs.erase(std::remove_if(xs.begin(), xs.end(),
                          [](double x) { return std::isnan(x); }),
           xs.end());
  if (xs.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(xs.begin(), xs.end());
  if (p <= 0.0) return xs.front();
  if (p >= 1.0) return xs.back();
  const double h = p * static_cast<double>(xs.size() - 1);
  const std::size_t i = static_cast<std::size_t>(h);
  if (i + 1 >= xs.size()) return xs.back();
  const double frac = h - static_cast<double>(i);
  return xs[i] + frac * (xs[i + 1] - xs[i]);
}

/* Exact quantile of a categorical-form gate_mixture: the generalised
 * inverse F⁻¹(p) = min{v : F(v) >= p} over the (value, mass) outcomes.
 * nullopt if an outcome's value fails to parse (falls to MC). */
std::optional<double> categorical_quantile(const GenericCircuit &gc,
                                           gate_t mix, double p)
{
  const auto &wires = gc.getWires(mix);
  std::vector<std::pair<double, double>> outcomes;
  outcomes.reserve(wires.size());
  for (std::size_t i = 1; i < wires.size(); ++i) {
    double v;
    try { v = parseDoubleStrict(gc.getExtra(wires[i])); }
    catch (const CircuitException &) { return std::nullopt; }
    outcomes.emplace_back(v, gc.getProb(wires[i]));
  }
  if (outcomes.empty()) return std::nullopt;
  std::sort(outcomes.begin(), outcomes.end());
  double cum = 0.0;
  for (const auto &vp : outcomes) {
    cum += vp.second;
    if (cum >= p && cum > 0.0) return vp.first;
  }
  return outcomes.back().first;   /* p ≈ 1 vs. mass-sum roundoff */
}

/* Closed-form (or numerically inverted) quantile of a bare gate_rv,
 * optionally truncated to [lo, hi] by a conditioning event: the
 * truncated quantile is Q(F(lo) + p·(F(hi) − F(lo))).  Tries the
 * family's elementary inverse CDF first, then the generic monotone
 * CDF bisection (Erlang / Gamma); nullopt when neither decides, so
 * the caller falls to MC. */
std::optional<double> analytic_dist_quantile(const Distribution &dist,
                                             double p, double lo, double hi);

std::optional<double> analytic_rv_quantile(const DistributionSpec &spec,
                                           double p, double lo, double hi)
{
  return analytic_dist_quantile(*makeDistribution(spec), p, lo, hi);
}

std::optional<double> analytic_dist_quantile(const Distribution &dist,
                                             double p, double lo, double hi)
{
  if (p <= 0.0 || p >= 1.0) {
    /* Quantile limits are the (truncated) support edges. */
    const auto sup = dist.support();
    return (p <= 0.0) ? std::max(sup.lo, lo) : std::min(sup.hi, hi);
  }
  double u = p;
  if (std::isfinite(lo) || std::isfinite(hi)) {
    const double f_lo = std::isfinite(lo) ? dist.cdf(lo) : 0.0;
    const double f_hi = std::isfinite(hi) ? dist.cdf(hi) : 1.0;
    if (std::isnan(f_lo) || std::isnan(f_hi)) return std::nullopt;
    const double mass = f_hi - f_lo;
    if (mass < 1e-12) return std::nullopt;   /* vanishing mass: MC's call */
    u = f_lo + p * mass;
  }
  double q = std::numeric_limits<double>::quiet_NaN();
  if (auto cf = dist.quantile(u)) q = *cf;
  if (std::isnan(q)) q = numericQuantile(dist, u);
  if (std::isnan(q)) return std::nullopt;
  /* Clamp defensively into the truncation interval (roundoff in u). */
  if (q < lo) q = lo;
  if (q > hi) q = hi;
  return q;
}

}  // namespace

double compute_quantile(const GenericCircuit &gc, gate_t root, double p,
                        std::optional<gate_t> event_root)
{
  const double inf = std::numeric_limits<double>::infinity();

  if (event_root.has_value()) {
    /* Continuous-density evidence: weighted empirical posterior quantile. */
    if (circuitHasObserve(gc, *event_root)) {
      const std::string what = "Posterior quantile";
      auto post = importanceSampleConditional(
                    gc, root, *event_root, mc_samples_or_throw(what));
      checkPosteriorOrThrow(post, what);
      return weightedQuantile(std::move(post), p);
    }
    /* Bare RV under an interval event: exact truncated quantile. */
    if (auto m = matchTruncatedSingleRv(gc, root, *event_root)) {
      if (auto q = analytic_rv_quantile(m->spec, p, m->lo, m->hi))
        return *q;
    }
    if (eventIsProvablyInfeasible(gc, root, *event_root))
      raise_infeasible_event(gc, root);
    auto cs = monteCarloConditionalScalarSamples(
                gc, root, *event_root,
                mc_samples_or_throw("Conditional quantile"));
    check_acceptance_or_throw(cs, "Conditional quantile");
    return empirical_quantile(std::move(cs.accepted), p);
  }

  const auto type = gc.getGateType(root);
  if (type == gate_value) {
    /* Dirac at c: every quantile is c. */
    try { return parseDoubleStrict(gc.getExtra(root)); }
    catch (const CircuitException &) { /* fall through to MC */ }
  } else if (type == gate_rv) {
    if (auto spec = parse_distribution_spec(gc.getExtra(root)))
      if (auto q = analytic_rv_quantile(*spec, p, -inf, inf))
        return *q;
  } else if (type == gate_mixture && gc.isCategoricalMixture(root)) {
    if (auto q = categorical_quantile(gc, root, p))
      return *q;
  } else if (type == gate_arith) {
    /* A unary LN / EXP transform, or a product of independent factors,
     * with a registered closed-form image (exp(normal) is lognormal,
     * lognormal products are lognormal, ...) has an exact quantile
     * through the image distribution. */
    const auto op = static_cast<provsql_arith_op>(gc.getInfos(root).first);
    std::unique_ptr<Distribution> image =
      (op == PROVSQL_ARITH_TIMES) ? product_image(gc, root)
                                  : transform_image(gc, root, op);
    if (image)
      if (auto q = analytic_dist_quantile(*image, p, -inf, inf))
        return *q;
  }

  /* Compound scalar circuits (arith trees, Bernoulli mixtures, ...):
   * quantiles do not decompose like moments, so estimate from the
   * empirical distribution at the rv_mc_samples budget. */
  return empirical_quantile(
    monteCarloScalarSamples(gc, root, mc_samples_or_throw("Quantile")),
    p);
}

/**
 * @brief Lift conditioning out of a scalar arithmetic expression.
 *
 * Implements @c "f(X|A, Y|B, …) = f(X, Y, …) | (A ∧ B ∧ …)": walks the scalar
 * tree rooted at @p root, replaces every nested @c gate_conditioned by a
 * transparent passthrough to its target (so the tree becomes the plain
 * arithmetic over the unconditioned distributions), collects the evidence
 * children, and conjoins them -- together with any pre-existing @p event_opt
 * -- into a single conditioning event.  The conjunction is built as an
 * in-memory @c gate_times over the evidence gates, all of which already live
 * in the (joint) circuit, so a base @c gate_rv shared between a value and its
 * evidence keeps a single draw under the MC sampler.  A conditioned ROOT is
 * peeled to its bare target (returned), so a stored "X | C" reaching any
 * low-level RV entry point keeps the closed-form scalar path; the (possibly
 * new) root is returned.  Leaves @p event_opt untouched and returns @p root
 * unchanged when the expression carries no conditioning.
 */
gate_t lift_conditioning(GenericCircuit &gc, gate_t root,
                         std::optional<gate_t> &event_opt)
{
  std::vector<gate_t> evidences;

  // 1. Peel a conditioned ROOT to its bare target.  A root has no parent
  //    wires, so it is replaced by its target directly rather than the
  //    single-child gate_arith passthrough the buried case below needs;
  //    keeping a bare gate_rv root preserves the closed-form truncation
  //    path for "X | (X > c)".  Handles the 2-child rv/agg carrier
  //    [target, condition] and (defensively) the 3-child uuid carrier
  //    [target, evidence, joint]; iterates in case of nested conditioning.
  while (gc.getGateType(root) == gate_conditioned) {
    const auto &w = gc.getWires(root);
    if (w.size() < 2)
      throw CircuitException("malformed conditioned gate in scalar expression");
    evidences.push_back(w[1]);
    root = w[0];
  }

  // 2. Replace every BURIED gate_conditioned by an arith passthrough to its
  //    target, collecting evidence as well.
  std::set<gate_t> seen;
  std::vector<gate_t> stack{root};
  while (!stack.empty()) {
    gate_t g = stack.back();
    stack.pop_back();
    if (!seen.insert(g).second) continue;
    if (gc.getGateType(g) == gate_conditioned) {
      const auto &w = gc.getWires(g);
      if (w.size() < 2)
        throw CircuitException("malformed conditioned gate in scalar expression");
      gate_t target = w[0];
      evidences.push_back(w[1]);
      gc.liftConditionedToTarget(g, target);  // g becomes arith PLUS [target]
      stack.push_back(target);
    } else {
      for (gate_t c : gc.getWires(g))
        stack.push_back(c);
    }
  }
  if (evidences.empty())
    return root;
  if (event_opt.has_value())
    evidences.push_back(*event_opt);
  gate_t cond;
  if (evidences.size() == 1)
    cond = evidences[0];
  else {
    cond = gc.setGate(gate_times);  // AND of all evidence (and prior event)
    auto &cw = gc.getWires(cond);
    for (gate_t e : evidences)
      cw.push_back(e);
  }
  event_opt = cond;
  return root;
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
/**
 * @brief SQL: agg_avg_moment_exact(token uuid, k integer) -> float8
 *
 * The exact independent-rows arm behind @c agg_raw_moment's @c avg
 * dispatch: E[AVG^k | COUNT >= 1] from the joint (sum, count) PMF over
 * pairwise leaf-disjoint contributors (@c aggAvgRawMomentExact).
 * Returns NULL when the shape is out of scope -- shared leaves, compound
 * contributors, unset probabilities -- and the SQL caller falls back to
 * the Monte-Carlo scalar path.
 */
Datum agg_avg_moment_exact(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    const int32 k_signed = PG_GETARG_INT32(1);

    if (k_signed < 0)
      provsql_error("agg_avg_moment_exact: k must be non-negative (got %d)",
                    k_signed);

    auto gc = getGenericCircuit(*token);
    gate_t root = gc.getGate(uuid2string(*token));
    bool ok = false;
    const double r = provsql::aggAvgRawMomentExact(
      gc, root, static_cast<unsigned>(k_signed), ok);
    if (!ok)
      PG_RETURN_NULL();
    return Float8GetDatum(r);
  } catch (const std::exception &e) {
    provsql_error("agg_avg_moment_exact: %s", e.what());
  } catch (...) {
    provsql_error("agg_avg_moment_exact: unknown exception");
  }
  PG_RETURN_NULL();
}

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

    /* Arithmetic over conditioned distributions: peel a conditioned ROOT to
     * its bare target (keeping the closed-form truncation path for the
     * bare-rv case) and lift any nested gate_conditioned out of the scalar
     * expression, folding its evidence into the conditioning event
     * (f(X|A, Y|B) = f(X, Y) | (A ∧ B)).  Works whether the token arrives
     * already unpacked by the SQL dispatcher or as a raw conditioned root
     * (Studio's distribution panel calls this low-level binding directly). */
    root_gate = provsql::lift_conditioning(gc, root_gate, event_opt);

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

/**
 * @brief SQL: rv_quantile(token uuid, p float8,
 *                         prov uuid DEFAULT gate_one()) -> float8
 *
 * C entry point behind the polymorphic @c quantile SQL dispatcher.
 * Same conditioning plumbing as @c rv_moment (joint circuit, nested
 * @c gate_conditioned lifting); the evaluation itself is
 * @c compute_quantile: closed-form / numerically inverted CDF for a
 * (possibly truncated) bare @c gate_rv, exact generalised inverse for
 * a categorical mixture, empirical MC quantile for compound circuits.
 */
Datum rv_quantile(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    const double p = PG_GETARG_FLOAT8(1);
    pg_uuid_t *prov = PG_GETARG_UUID_P(2);

    if (std::isnan(p) || p < 0.0 || p > 1.0)
      provsql_error("rv_quantile: p must be in [0, 1] (got %g)", p);

    gate_t root_gate, event_gate;
    auto gc = getJointCircuit(*token, *prov, root_gate, event_gate);

    /* gate_one event = unconditional after load-time simplification. */
    std::optional<gate_t> event_opt;
    if (gc.getGateType(event_gate) != gate_one)
      event_opt = event_gate;

    root_gate = provsql::lift_conditioning(gc, root_gate, event_opt);

    return Float8GetDatum(
      provsql::compute_quantile(gc, root_gate, p, event_opt));
  } catch (const std::exception &e) {
    provsql_error("rv_quantile: %s", e.what());
  } catch (...) {
    provsql_error("rv_quantile: unknown exception");
  }
  PG_RETURN_NULL();
}

/**
 * @brief SQL: rv_evidence(evidence uuid) -> float8
 *
 * The marginal likelihood @c P(data) of an evidence circuit: the mean raw
 * importance weight over @c provsql.rv_mc_samples prior draws (the same
 * quantity rejection conditioning computes as @c P(C), now a product of the
 * observations' densities).  Backs @c provsql.evidence.
 */
Datum rv_evidence(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    auto gc = getGenericCircuit(*token);
    gate_t root = gc.getGate(uuid2string(*token));
    if (provsql_rv_mc_samples == 0)
      provsql_error(
        "rv_evidence: provsql.rv_mc_samples is 0 (the marginal likelihood is "
        "estimated by Monte Carlo); set it to a positive sample budget");
    const double e = provsql::importanceEvidence(
      gc, root, static_cast<unsigned>(provsql_rv_mc_samples));
    return Float8GetDatum(e);
  } catch (const std::exception &ex) {
    provsql_error("rv_evidence: %s", ex.what());
  } catch (...) {
    provsql_error("rv_evidence: unknown exception");
  }
  PG_RETURN_NULL();
}

}  // extern "C"
