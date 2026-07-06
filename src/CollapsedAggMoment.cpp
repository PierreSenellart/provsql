/**
 * @file CollapsedAggMoment.cpp
 * @brief Rao-Blackwellised (collapsed) moments of a correlated COUNT / SUM,
 *        and the exact posterior of a latent conditioned on such a count.
 *
 * The recurring shape in latent-variable relational models is an aggregate
 * over probabilistically-selected rows whose selection events are coupled by
 * a small shared latent:
 *
 *   C = COUNT/SUM over rows i of  [ eps_i  op  g_i(b) ]
 *
 * where @c eps_i is a per-row independent random-variable leaf, @c b is a
 * shared continuous latent that every row references, and @c g_i(b) is an
 * expression in @c b (plus per-row constants).  The exact moment path
 * enumerates @c n^k row tuples and evaluates a joint probability for each --
 * @c O(n^2) pair-probabilities for the variance, which does not scale.
 *
 * Conditional on @c b, the indicators are INDEPENDENT (the only coupling is
 * through @c b), so this collapses to a 1-D quadrature over @c b: at each node
 * the per-row success probability @c q_i(b) = Pr[eps_i op g_i(b)] is a
 * closed-form CDF, and the count's conditional law is the elementary
 * sum-of-independent-Bernoulli (Poisson-binomial) distribution.  Cost
 * @c O(G·n) (moments) / @c O(G·n^2) (pmf) -- milliseconds where the exact path
 * took minutes -- and exact up to the quadrature (no sampling).
 *
 * The same collapse yields the EXACT posterior of a latent @c R conditioned on
 * a discrete rv @c Y(R) equalling the count: @c P(C=j) is the collapsed pmf,
 * and @c E[R^k|C] is a 1-D quadrature over @c R weighted by the likelihood
 * @c L(r)=Σ_j P(C=j) pmf_Y(j;θ(r)) -- replacing a degenerating importance
 * sampler with a closed quadrature.
 *
 * Scope (returns @c std::nullopt, i.e. "fall back", on any mismatch): a
 * @c COUNT / @c SUM @c gate_agg whose per-row indicators reduce to
 * @c "(constant Bernoullis) AND (one gate_cmp)", the cmp comparing a bare
 * per-row @c gate_rv leaf against an expression over exactly ONE shared
 * continuous latent leaf.  Everything else declines.
 */
#include "CollapsedAggMoment.h"

#include "CircuitFromMMap.h"
#include "RandomVariable.h"
#include "Aggregation.h"                 // AggregationOperator, ComparisonOperator
#include "distributions/Distribution.h"  // makeDistribution
#include "Circuit.h"
#include "provsql_utils_cpp.h"           // uuid2string

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/uuid.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "provsql_utils.h"
#include "provsql_error.h"
}

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace provsql {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kGrid = 400;   ///< quadrature nodes (shared latent and posterior)

/// Collect the @c gate_rv leaves reachable from @p g (the RV footprint).
void collectRvLeaves(const GenericCircuit &gc, gate_t g,
                     std::unordered_set<gate_t> &out)
{
  std::unordered_set<gate_t> seen;
  std::vector<gate_t> stack{g};
  while (!stack.empty()) {
    gate_t x = stack.back(); stack.pop_back();
    if (!seen.insert(x).second) continue;
    if (gc.getGateType(x) == gate_rv) { out.insert(x); continue; }
    for (gate_t c : gc.getWires(x)) stack.push_back(c);
  }
}

/// Evaluate a deterministic scalar expression at @p bval, where the only
/// random leaf allowed is @p var (set to @p bval).  Returns NaN on any
/// unexpected gate, which aborts the collapse.  Used both for the shared-side
/// link expression g_i(b) and for a latent leaf's parameter expression θ(r).
double evalWithVar(const GenericCircuit &gc, gate_t g, gate_t var, double val)
{
  switch (gc.getGateType(g)) {
    case gate_rv:
      return (g == var) ? val : kNaN;
    case gate_value:
      try { return parseDoubleStrict(gc.getExtra(g)); }
      catch (const CircuitException &) { return kNaN; }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &w = gc.getWires(g);
      auto ev = [&](gate_t c) { return evalWithVar(gc, c, var, val); };
      switch (op) {
        case PROVSQL_ARITH_PLUS: {
          double s = 0.0; for (gate_t c : w) s += ev(c); return s;
        }
        case PROVSQL_ARITH_TIMES: {
          double p = 1.0; for (gate_t c : w) p *= ev(c); return p;
        }
        case PROVSQL_ARITH_MINUS:
          return (w.size() == 2) ? ev(w[0]) - ev(w[1]) : kNaN;
        case PROVSQL_ARITH_DIV:
          return (w.size() == 2) ? ev(w[0]) / ev(w[1]) : kNaN;
        case PROVSQL_ARITH_NEG:
          return (w.size() == 1) ? -ev(w[0]) : kNaN;
        default:
          return kNaN;
      }
    }
    default:
      return kNaN;
  }
}

/// Resolve @p g to an affine form `slope·var + intercept` over the same ops
/// @c evalWithVar handles, or std::nullopt when it is not affine in @p var
/// (a genuinely non-linear b-dependence, or an unsupported gate).  A structural
/// (not sampled) decomposition, so it never mis-classifies a non-linear link.
/// In a shared-side link expression the only rv leaf present is @p var, so a
/// @c gate_rv other than @p var cannot occur (it declines defensively anyway).
std::optional<std::pair<double, double>>
affineInVar(const GenericCircuit &gc, gate_t g, gate_t var)
{
  using R = std::optional<std::pair<double, double>>;
  switch (gc.getGateType(g)) {
    case gate_rv:
      return (g == var) ? R({1.0, 0.0}) : std::nullopt;
    case gate_value:
      try { return R({0.0, parseDoubleStrict(gc.getExtra(g))}); }
      catch (const CircuitException &) { return std::nullopt; }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &w = gc.getWires(g);
      switch (op) {
        case PROVSQL_ARITH_PLUS: {
          double s = 0.0, c = 0.0;
          for (gate_t ch : w) {
            auto a = affineInVar(gc, ch, var);
            if (!a) return std::nullopt;
            s += a->first; c += a->second;
          }
          return R({s, c});
        }
        case PROVSQL_ARITH_TIMES: {
          double s = 0.0, c = 1.0;   // running affine product (starts at 1)
          for (gate_t ch : w) {
            auto a = affineInVar(gc, ch, var);
            if (!a) return std::nullopt;
            // (s·x + c)·(a.s·x + a.c) is affine only if a factor is constant.
            if (s != 0.0 && a->first != 0.0) return std::nullopt;
            const double ns = s * a->second + c * a->first;
            c = c * a->second;
            s = ns;
          }
          return R({s, c});
        }
        case PROVSQL_ARITH_MINUS: {
          if (w.size() != 2) return std::nullopt;
          auto a = affineInVar(gc, w[0], var), b = affineInVar(gc, w[1], var);
          if (!a || !b) return std::nullopt;
          return R({a->first - b->first, a->second - b->second});
        }
        case PROVSQL_ARITH_NEG: {
          if (w.size() != 1) return std::nullopt;
          auto a = affineInVar(gc, w[0], var);
          if (!a) return std::nullopt;
          return R({-a->first, -a->second});
        }
        case PROVSQL_ARITH_DIV: {
          if (w.size() != 2) return std::nullopt;
          auto a = affineInVar(gc, w[0], var), b = affineInVar(gc, w[1], var);
          if (!a || !b) return std::nullopt;
          if (b->first != 0.0 || b->second == 0.0) return std::nullopt;
          return R({a->first / b->second, a->second / b->second});
        }
        default:
          return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

/// One recognised per-row term: q_i(b) = bern · Pr[eps_i op_i g_i(b)].
struct Term {
  const DistributionFamily *family;  ///< eps_i family (for the CDF)
  double p1, p2;                     ///< eps_i parameters
  gate_t shared_side;                ///< g_i(b) expression gate
  ComparisonOperator op;             ///< comparison, oriented "eps_i op g_i"
  double bern;                       ///< product of the constant Bernoulli inputs
  double value;                      ///< v_i (1 for COUNT; the summed value for SUM)

  // Precomputed, b-invariant, so the O(G·n) grid loop touches no gate: the
  // eps_i distribution is built once (its parameters are constant), and the
  // shared-side link g_i(b) is resolved to an affine form slope·b + intercept
  // whenever it is affine in the shared latent (the common alpha_i + b link),
  // sparing the per-node evalWithVar walk and its std::map gate lookups.
  std::shared_ptr<Distribution> dist;  ///< family->factory(p1, p2), cached
  bool   affine = false;               ///< g_i(b) = slope·b + intercept
  double slope = 0.0, intercept = 0.0; ///< valid iff affine
};

/// The recognised collapse: the shared latent, its distribution and quadrature
/// window, and the oriented per-row terms.
struct CollapsePlan {
  AggregationOperator aop;
  gate_t shared;
  std::unique_ptr<Distribution> shared_dist;
  double lo, hi;
  std::vector<Term> terms;
};

/// Reduce an indicator k-gate to (Bernoulli product, the single gate_cmp).
/// Returns false if it is not "(constant inputs) AND (one cmp)".
bool reduceIndicator(const GenericCircuit &gc, gate_t k_gate,
                     double &bern, gate_t &cmp_out)
{
  bern = 1.0;
  bool have_cmp = false;
  std::vector<gate_t> stack{k_gate};
  while (!stack.empty()) {
    gate_t g = stack.back(); stack.pop_back();
    switch (gc.getGateType(g)) {
      case gate_times:
        for (gate_t c : gc.getWires(g)) stack.push_back(c);
        break;
      case gate_one:
        break;
      case gate_input:
      case gate_update: {
        const double p = gc.getProb(g);
        if (std::isnan(p)) return false;
        bern *= p;
        break;
      }
      case gate_cmp:
        if (have_cmp) return false;   // only a single comparison supported
        cmp_out = g;
        have_cmp = true;
        break;
      default:
        return false;
    }
  }
  return have_cmp;
}

ComparisonOperator flipOp(ComparisonOperator op)
{
  switch (op) {
    case ComparisonOperator::LT: return ComparisonOperator::GT;
    case ComparisonOperator::LE: return ComparisonOperator::GE;
    case ComparisonOperator::GT: return ComparisonOperator::LT;
    case ComparisonOperator::GE: return ComparisonOperator::LE;
    default:                     return op;   // EQ / NE symmetric
  }
}

/// Recognise the shared-latent COUNT / SUM shape at @p agg and build the plan.
std::optional<CollapsePlan>
buildCollapsePlan(const GenericCircuit &gc, gate_t agg)
{
  if (gc.getGateType(agg) != gate_agg) return std::nullopt;
  const AggregationOperator aop = getAggregationOperator(gc.getInfos(agg).first);
  if (aop != AggregationOperator::COUNT && aop != AggregationOperator::SUM)
    return std::nullopt;

  const auto &children = gc.getWires(agg);
  if (children.empty()) return std::nullopt;

  struct Raw {
    double bern, value;
    gate_t a, b;
    ComparisonOperator op;
  };
  std::vector<Raw> raws;
  raws.reserve(children.size());
  std::unordered_map<gate_t, unsigned> leaf_rows;

  for (gate_t sm : children) {
    if (gc.getGateType(sm) != gate_semimod) return std::nullopt;
    const auto &smw = gc.getWires(sm);
    if (smw.size() != 2) return std::nullopt;
    if (gc.getGateType(smw[1]) != gate_value) return std::nullopt;
    double value;
    try { value = parseDoubleStrict(gc.getExtra(smw[1])); }
    catch (const CircuitException &) { return std::nullopt; }

    double bern;
    gate_t cmp = static_cast<gate_t>(0);
    if (!reduceIndicator(gc, smw[0], bern, cmp)) return std::nullopt;
    const auto &cw = gc.getWires(cmp);
    if (cw.size() != 2) return std::nullopt;
    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(cmp).first, ok);
    if (!ok) return std::nullopt;

    raws.push_back({bern, value, cw[0], cw[1], op});

    std::unordered_set<gate_t> rowleaves;
    collectRvLeaves(gc, cmp, rowleaves);
    for (gate_t l : rowleaves) ++leaf_rows[l];
  }

  gate_t shared = static_cast<gate_t>(0);
  unsigned nb_shared = 0;
  for (const auto &[leaf, rows] : leaf_rows)
    if (rows > 1) { shared = leaf; ++nb_shared; }
  if (nb_shared != 1) return std::nullopt;

  auto shared_spec = parse_distribution_spec(gc.getExtra(shared));
  if (!shared_spec) return std::nullopt;
  auto shared_dist = makeDistribution(*shared_spec);
  double lo, hi;
  if (!shared_dist->integrationRange(lo, hi) || !(hi > lo)) return std::nullopt;

  std::vector<Term> terms;
  terms.reserve(raws.size());
  for (const auto &r : raws) {
    std::unordered_set<gate_t> fa, fb;
    collectRvLeaves(gc, r.a, fa);
    collectRvLeaves(gc, r.b, fb);

    gate_t per_row = static_cast<gate_t>(0),
           shared_side = static_cast<gate_t>(0);
    ComparisonOperator op = r.op;
    auto is_per_row = [&](gate_t side, const std::unordered_set<gate_t> &f) {
      return gc.getGateType(side) == gate_rv && side != shared && f.size() == 1;
    };
    auto is_shared_side = [&](const std::unordered_set<gate_t> &f) {
      return f.empty() || (f.size() == 1 && f.count(shared));
    };
    if (is_per_row(r.a, fa) && is_shared_side(fb)) {
      per_row = r.a; shared_side = r.b;
    } else if (is_per_row(r.b, fb) && is_shared_side(fa)) {
      per_row = r.b; shared_side = r.a; op = flipOp(op);
    } else {
      return std::nullopt;
    }
    if (op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
      return std::nullopt;

    auto spec = parse_distribution_spec(gc.getExtra(per_row));
    if (!spec) return std::nullopt;
    Term term{spec->family, spec->p1, spec->p2, shared_side, op,
              r.bern, r.value};
    term.dist = std::shared_ptr<Distribution>(
                  spec->family->factory(spec->p1, spec->p2));
    if (auto aff = affineInVar(gc, shared_side, shared)) {
      term.affine = true;
      term.slope = aff->first;
      term.intercept = aff->second;
    }
    terms.push_back(std::move(term));
  }

  CollapsePlan plan;
  plan.aop = aop;
  plan.shared = shared;
  plan.shared_dist = std::move(shared_dist);
  plan.lo = lo;
  plan.hi = hi;
  plan.terms = std::move(terms);
  return plan;
}

/// The trapezoidal quadrature grid over a latent's distribution, weighted by
/// its pdf and renormalised to unit mass (removing discretisation bias).
struct Grid { std::vector<double> node, weight; };
std::optional<Grid> pdfGrid(const Distribution &d, double lo, double hi)
{
  if (!(hi > lo)) return std::nullopt;
  const double dx = (hi - lo) / (kGrid - 1);
  Grid grid;
  grid.node.resize(kGrid);
  grid.weight.resize(kGrid);
  double wsum = 0.0;
  for (int gi = 0; gi < kGrid; ++gi) {
    grid.node[gi] = lo + gi * dx;
    double w = d.pdf(grid.node[gi]);
    if (std::isnan(w) || w < 0.0) w = 0.0;
    if (gi == 0 || gi == kGrid - 1) w *= 0.5;
    grid.weight[gi] = w;
    wsum += w;
  }
  if (!(wsum > 0.0)) return std::nullopt;
  for (double &w : grid.weight) w /= wsum;
  return grid;
}

/// Per-row success probabilities q_i(b) at a shared-latent value @p b.
/// Returns false if any term's link expression / CDF is undefined.
bool perNodeProbs(const GenericCircuit &gc, const CollapsePlan &plan,
                  double b, std::vector<double> &q)
{
  q.clear();
  q.reserve(plan.terms.size());
  for (const auto &t : plan.terms) {
    // Affine links resolve with no gate access; the rest keep the walk.
    const double c = t.affine ? std::fma(t.slope, b, t.intercept)
                              : evalWithVar(gc, t.shared_side, plan.shared, b);
    if (std::isnan(c)) return false;
    double F = t.dist->cdf(c);
    if (std::isnan(F)) return false;
    if (F < 0.0) F = 0.0; else if (F > 1.0) F = 1.0;
    double qi;
    switch (t.op) {
      case ComparisonOperator::LT:
      case ComparisonOperator::LE: qi = F;        break;
      case ComparisonOperator::GT:
      case ComparisonOperator::GE: qi = 1.0 - F;  break;
      default: return false;
    }
    qi *= t.bern;
    if (qi < 0.0) qi = 0.0; else if (qi > 1.0) qi = 1.0;
    q.push_back(qi);
  }
  return true;
}

/// Poisson-binomial pmf of Σ Bernoulli(q_i) on {0, ..., q.size()}.
std::vector<double> poissonBinomialPmf(const std::vector<double> &q)
{
  std::vector<double> pmf(q.size() + 1, 0.0);
  pmf[0] = 1.0;
  std::size_t filled = 0;
  for (double qi : q) {
    // Fold in Bernoulli(qi): pmf'[j] = pmf[j](1-qi) + pmf[j-1] qi, updated in
    // place descending so each pmf[j-1] is still the pre-fold value.
    for (std::size_t j = filled + 1; j >= 1; --j)
      pmf[j] = pmf[j] * (1.0 - qi) + pmf[j - 1] * qi;
    pmf[0] *= (1.0 - qi);
    ++filled;
  }
  return pmf;
}

/// The count pmf P(C=j) via the collapse: 1-D quadrature over the shared
/// latent of the conditional Poisson-binomial.  COUNT only (unit per-row
/// values), so the sum is a plain count.
std::optional<std::vector<double>>
aggCollapsedPmf(const GenericCircuit &gc, gate_t agg)
{
  auto plan = buildCollapsePlan(gc, agg);
  if (!plan) return std::nullopt;
  // A count is a COUNT agg, or equivalently a SUM whose per-row values are all
  // 1 (the count-lift shape).  Either way the per-tuple contribution is 1, so
  // the Poisson-binomial pmf is the count distribution.
  for (const auto &t : plan->terms)
    if (t.value != 1.0) return std::nullopt;

  auto grid = pdfGrid(*plan->shared_dist, plan->lo, plan->hi);
  if (!grid) return std::nullopt;

  const std::size_t n = plan->terms.size();
  std::vector<double> pmf(n + 1, 0.0);
  std::vector<double> q;
  for (int gi = 0; gi < kGrid; ++gi) {
    if (!perNodeProbs(gc, *plan, grid->node[gi], q)) return std::nullopt;
    const std::vector<double> pb = poissonBinomialPmf(q);
    const double w = grid->weight[gi];
    for (std::size_t j = 0; j <= n; ++j) pmf[j] += w * pb[j];
  }
  return pmf;
}

}  // namespace

std::optional<std::pair<double, double>>
aggCollapsedRawMoments(const GenericCircuit &gc, gate_t agg)
{
  auto plan = buildCollapsePlan(gc, agg);
  if (!plan) return std::nullopt;

  auto grid = pdfGrid(*plan->shared_dist, plan->lo, plan->hi);
  if (!grid) return std::nullopt;

  double m1 = 0.0, m2 = 0.0;
  std::vector<double> q;
  for (int gi = 0; gi < kGrid; ++gi) {
    if (!perNodeProbs(gc, *plan, grid->node[gi], q)) return std::nullopt;
    double mean_b = 0.0, var_b = 0.0;   // E[C|b], Var[C|b] (independent given b)
    for (std::size_t i = 0; i < plan->terms.size(); ++i) {
      const double v = plan->terms[i].value, qi = q[i];
      mean_b += v * qi;
      var_b  += v * v * qi * (1.0 - qi);
    }
    const double w = grid->weight[gi];
    m1 += w * mean_b;
    m2 += w * (var_b + mean_b * mean_b);
  }
  return std::make_pair(m1, m2);
}

std::optional<double>
aggCollapsedRawMoment(const GenericCircuit &gc, gate_t agg, unsigned k)
{
  if (k == 0) return 1.0;
  if (k > 2) return std::nullopt;
  // Both moments share the whole load / plan / grid pass; variance() consumes
  // them together through agg_collapsed_moments so a readout that needs E[C]
  // and E[C^2] pays for one traversal, not two.
  auto m = aggCollapsedRawMoments(gc, agg);
  if (!m) return std::nullopt;
  return (k == 1) ? m->first : m->second;
}

std::optional<double>
collapsedConditionalMoment(const GenericCircuit &gc, gate_t target,
                           gate_t event, unsigned k)
{
  if (k == 0) return 1.0;
  if (k > 2) return std::nullopt;
  if (gc.getGateType(target) != gate_rv) return std::nullopt;
  if (gc.getGateType(event) != gate_cmp) return std::nullopt;

  bool ok = false;
  const ComparisonOperator eop = cmpOpFromOid(gc.getInfos(event).first, ok);
  if (!ok || eop != ComparisonOperator::EQ) return std::nullopt;
  const auto &ew = gc.getWires(event);
  if (ew.size() != 2) return std::nullopt;

  // Identify the discrete rv Y(target) side and the count agg C side.
  gate_t ygate = static_cast<gate_t>(0), cagg = static_cast<gate_t>(0);
  for (int i = 0; i < 2; ++i) {
    const gate_t s = ew[i], o = ew[1 - i];
    if (gc.getGateType(s) == gate_rv && gc.getGateType(o) == gate_agg) {
      ygate = s; cagg = o; break;
    }
  }
  if (ygate == static_cast<gate_t>(0)) return std::nullopt;

  // Y must be a discrete family whose parameter subtree references target.
  auto ytmpl = parse_distribution_template(gc.getExtra(ygate));
  if (!ytmpl) return std::nullopt;
  if (!ytmpl->family->factory(1.0, 1.0)->isDiscrete()) return std::nullopt;
  // Y's parameter subtrees are its wires; descend into them (collectRvLeaves
  // stops at the gate_rv Y itself) to confirm the parameter references target.
  std::unordered_set<gate_t> yfoot;
  for (gate_t w : gc.getWires(ygate)) collectRvLeaves(gc, w, yfoot);
  if (!yfoot.count(target)) return std::nullopt;

  // The correlated count's pmf via the collapse.
  auto Cpmf = aggCollapsedPmf(gc, cagg);
  if (!Cpmf) return std::nullopt;

  // R's prior and quadrature window.
  auto rspec = parse_distribution_spec(gc.getExtra(target));
  if (!rspec) return std::nullopt;
  auto rdist = makeDistribution(*rspec);
  double rlo, rhi;
  if (!rdist->integrationRange(rlo, rhi)) return std::nullopt;
  auto rgrid = pdfGrid(*rdist, rlo, rhi);
  if (!rgrid) return std::nullopt;

  const auto &ywires = gc.getWires(ygate);
  auto resolveParam = [&](const DistributionParam &p, double r) -> double {
    if (p.wire_slot < 0) return p.literal;
    if (static_cast<std::size_t>(p.wire_slot) >= ywires.size()) return kNaN;
    return evalWithVar(gc, ywires[p.wire_slot], target, r);
  };

  // Posterior quadrature over R: E[R^k|C] = ∫ r^k f_R L / ∫ f_R L,
  // L(r) = Σ_j P(C=j) pmf_Y(j; θ(r)).
  const std::size_t J = Cpmf->size();
  double Z = 0.0, N1 = 0.0, N2 = 0.0;
  for (int gi = 0; gi < kGrid; ++gi) {
    const double r = rgrid->node[gi];
    const double p1v = resolveParam(ytmpl->p1, r);
    const double p2v = resolveParam(ytmpl->p2, r);
    if (std::isnan(p1v)) return std::nullopt;
    auto ydist = ytmpl->family->factory(p1v, std::isnan(p2v) ? 0.0 : p2v);
    double L = 0.0;
    bool degenerate = false;
    for (std::size_t j = 0; j < J; ++j) {
      const double pj = (*Cpmf)[j];
      if (pj <= 0.0) continue;
      const double pmfy = ydist->pdf(static_cast<double>(j));
      if (std::isnan(pmfy)) { degenerate = true; break; }
      L += pj * pmfy;
    }
    // A degenerate Y at this node (e.g. a boundary parameter such as
    // Poisson(0) at R = 0) contributes nothing: such nodes sit at the
    // prior's support edge, where f_R already vanishes, so skipping them
    // leaves the quadrature exact.
    if (degenerate) continue;
    const double w = rgrid->weight[gi];
    Z  += w * L;
    N1 += w * r * L;
    N2 += w * r * r * L;
  }
  if (!(Z > 0.0)) return std::nullopt;
  return (k == 1) ? (N1 / Z) : (N2 / Z);
}

}  // namespace provsql

extern "C" {

PG_FUNCTION_INFO_V1(agg_collapsed_moment);

/**
 * @brief SQL: agg_collapsed_moment(token uuid, k integer) -> float8
 *
 * The collapsed raw moment @c E[C^k] of a correlated COUNT / SUM, or @c NULL
 * when the circuit does not match the recognised shared-latent pattern (the
 * caller then falls back to the exact enumeration).  @c k in @c {1, 2}.
 */
Datum agg_collapsed_moment(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    const int32 k = PG_GETARG_INT32(1);
    if (k < 0)
      provsql_error("agg_collapsed_moment: k must be non-negative (got %d)", k);
    auto gc = getGenericCircuit(*token);
    gate_t root = gc.getGate(uuid2string(*token));
    auto r = provsql::aggCollapsedRawMoment(gc, root,
                                            static_cast<unsigned>(k));
    if (!r.has_value())
      PG_RETURN_NULL();
    return Float8GetDatum(*r);
  } catch (const std::exception &e) {
    provsql_error("agg_collapsed_moment: %s", e.what());
  } catch (...) {
    provsql_error("agg_collapsed_moment: unknown exception");
  }
  PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(agg_collapsed_moments);

/**
 * @brief SQL: agg_collapsed_moments(token uuid) -> float8[] {E[C], E[C^2]}
 *
 * Both collapsed raw moments from a single circuit load and plan build, or
 * @c NULL when the shared-latent pattern does not match.  @c variance() calls
 * this so a mean+variance readout traverses the circuit once, not twice (the
 * load and plan build dominate once the grid loop is arithmetic-only).
 */
Datum agg_collapsed_moments(PG_FUNCTION_ARGS)
{
  try {
    pg_uuid_t *token = PG_GETARG_UUID_P(0);
    auto gc = getGenericCircuit(*token);
    gate_t root = gc.getGate(uuid2string(*token));
    auto m = provsql::aggCollapsedRawMoments(gc, root);
    if (!m.has_value())
      PG_RETURN_NULL();
    Datum elems[2] = { Float8GetDatum(m->first), Float8GetDatum(m->second) };
    ArrayType *arr = construct_array(elems, 2, FLOAT8OID,
                                     sizeof(float8), FLOAT8PASSBYVAL, 'd');
    PG_RETURN_ARRAYTYPE_P(arr);
  } catch (const std::exception &e) {
    provsql_error("agg_collapsed_moments: %s", e.what());
  } catch (...) {
    provsql_error("agg_collapsed_moments: unknown exception");
  }
  PG_RETURN_NULL();
}

}  // extern "C"
