/**
 * @file CollapsedAggMoment.cpp
 * @brief Rao-Blackwellised (collapsed) moments of a correlated COUNT / SUM.
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
 * through @c b), so this collapses to a 1-D quadrature over @c b: at each
 * node the per-row success probability @c q_i(b) = Pr[eps_i op g_i(b)] is a
 * closed-form CDF, and the count's conditional moments are the elementary
 * sum-of-independent-Bernoulli formulas.  Cost @c O(G·n) for @c G grid nodes
 * -- milliseconds where the exact path took minutes -- and it is exact up to
 * the quadrature (no sampling).
 *
 * Scope (returns @c std::nullopt, i.e. "fall back to the exact path", on any
 * mismatch): a @c COUNT / @c SUM @c gate_agg whose per-row indicators reduce
 * to @c "(constant Bernoullis) AND (one gate_cmp)", the cmp comparing a bare
 * per-row @c gate_rv leaf against an expression over exactly ONE shared
 * continuous latent leaf; @c k in @c {1, 2}.  Everything else declines.
 */
#include "GenericCircuit.h"
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
#include "provsql_utils.h"
#include "provsql_error.h"
}

#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace provsql {

namespace {

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

/// Evaluate a deterministic "shared side" expression at @p bval, where the
/// only random leaf allowed is the shared latent @p shared (set to @p bval).
/// Returns NaN on any unexpected gate, which aborts the collapse.
double evalSharedSide(const GenericCircuit &gc, gate_t g, gate_t shared,
                      double bval)
{
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  switch (gc.getGateType(g)) {
    case gate_rv:
      return (g == shared) ? bval : kNaN;
    case gate_value:
      try { return parseDoubleStrict(gc.getExtra(g)); }
      catch (const CircuitException &) { return kNaN; }
    case gate_arith: {
      const auto op = static_cast<provsql_arith_op>(gc.getInfos(g).first);
      const auto &w = gc.getWires(g);
      auto ev = [&](gate_t c) { return evalSharedSide(gc, c, shared, bval); };
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
          return kNaN;   // MAX/MIN/POW/... not expected on a link expression
      }
    }
    default:
      return kNaN;
  }
}

/// One recognised per-row term: q_i(b) = p_i · Pr[eps_i op_i g_i(b)].
struct Term {
  const DistributionFamily *family;  ///< eps_i family (for the CDF)
  double p1, p2;                     ///< eps_i parameters
  gate_t shared_side;                ///< g_i(b) expression gate
  ComparisonOperator op;             ///< comparison, oriented "eps_i op g_i"
  double bern;                       ///< product of the constant Bernoulli inputs
  double value;                      ///< v_i (1 for COUNT; the summed value for SUM)
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

}  // namespace

std::optional<double>
aggCollapsedRawMoment(const GenericCircuit &gc, gate_t agg, unsigned k)
{
  if (k == 0) return 1.0;
  if (k > 2) return std::nullopt;                 // k>=3 needs the PB DP; defer
  if (gc.getGateType(agg) != gate_agg) return std::nullopt;

  const AggregationOperator aop = getAggregationOperator(gc.getInfos(agg).first);
  if (aop != AggregationOperator::COUNT && aop != AggregationOperator::SUM)
    return std::nullopt;

  const auto &children = gc.getWires(agg);
  const std::size_t n = children.size();
  if (n == 0) return std::nullopt;

  /* Pass 1: reduce each indicator to (Bernoulli, cmp, sides), and count how
   * often each rv leaf appears so the shared latent is the one common to
   * multiple rows. */
  struct Raw {
    double bern, value;
    gate_t cmp;
    gate_t a, b;                 // the two cmp operands
    ComparisonOperator op;
  };
  std::vector<Raw> raws;
  raws.reserve(n);
  std::unordered_map<gate_t, unsigned> leaf_rows;   // leaf -> #rows it appears in

  for (gate_t sm : children) {
    if (gc.getGateType(sm) != gate_semimod) return std::nullopt;
    const auto &smw = gc.getWires(sm);
    if (smw.size() != 2) return std::nullopt;
    if (gc.getGateType(smw[1]) != gate_value) return std::nullopt;
    double value;
    try { value = parseDoubleStrict(gc.getExtra(smw[1])); }
    catch (const CircuitException &) { return std::nullopt; }

    double bern;
    gate_t cmp;
    if (!reduceIndicator(gc, smw[0], bern, cmp)) return std::nullopt;
    const auto &cw = gc.getWires(cmp);
    if (cw.size() != 2) return std::nullopt;
    bool ok = false;
    ComparisonOperator op = cmpOpFromOid(gc.getInfos(cmp).first, ok);
    if (!ok) return std::nullopt;

    raws.push_back({bern, value, cmp, cw[0], cw[1], op});

    std::unordered_set<gate_t> rowleaves;
    collectRvLeaves(gc, cmp, rowleaves);
    for (gate_t l : rowleaves) ++leaf_rows[l];
  }

  /* The shared latent: the unique rv leaf appearing in more than one row.
   * Require exactly one (a single 1-D coupling block). */
  gate_t shared = static_cast<gate_t>(0);
  unsigned nb_shared = 0;
  for (const auto &[leaf, rows] : leaf_rows)
    if (rows > 1) { shared = leaf; ++nb_shared; }
  if (nb_shared != 1) return std::nullopt;

  auto shared_spec = parse_distribution_spec(gc.getExtra(shared));
  if (!shared_spec) return std::nullopt;           // parametric / malformed
  auto shared_dist = makeDistribution(*shared_spec);
  double lo, hi;
  if (!shared_dist->integrationRange(lo, hi)) return std::nullopt;

  /* Pass 2: orient each cmp as "eps_i op g_i(b)", with eps_i a bare per-row
   * gate_rv leaf and g_i the shared-latent-only side. */
  std::vector<Term> terms;
  terms.reserve(n);
  for (const auto &r : raws) {
    std::unordered_set<gate_t> fa, fb;
    collectRvLeaves(gc, r.a, fa);
    collectRvLeaves(gc, r.b, fb);

    /* per-row side: a bare gate_rv leaf that is NOT the shared latent;
     * shared side: footprint ⊆ {shared}. */
    gate_t per_row = static_cast<gate_t>(0),
           shared_side = static_cast<gate_t>(0);
    ComparisonOperator op = r.op;
    auto is_per_row = [&](gate_t side, const std::unordered_set<gate_t> &f) {
      return gc.getGateType(side) == gate_rv && side != shared
             && f.size() == 1;
    };
    auto is_shared_side = [&](const std::unordered_set<gate_t> &f) {
      return f.empty() || (f.size() == 1 && f.count(shared));
    };
    if (is_per_row(r.a, fa) && is_shared_side(fb)) {
      per_row = r.a; shared_side = r.b;             // eps op g
    } else if (is_per_row(r.b, fb) && is_shared_side(fa)) {
      per_row = r.b; shared_side = r.a; op = flipOp(op);   // g op eps -> eps op' g
    } else {
      return std::nullopt;
    }
    if (op == ComparisonOperator::EQ || op == ComparisonOperator::NE)
      return std::nullopt;   // continuous point event: not this pattern

    auto spec = parse_distribution_spec(gc.getExtra(per_row));
    if (!spec) return std::nullopt;
    terms.push_back({spec->family, spec->p1, spec->p2, shared_side, op,
                     r.bern, r.value});
  }

  /* 1-D quadrature over the shared latent.  Trapezoidal grid weighted by the
   * latent's pdf, renormalised to unit mass to remove discretisation bias. */
  constexpr int G = 400;
  if (!(hi > lo)) return std::nullopt;
  const double dx = (hi - lo) / (G - 1);

  std::vector<double> node(G), weight(G);
  double wsum = 0.0;
  for (int gi = 0; gi < G; ++gi) {
    node[gi] = lo + gi * dx;
    double w = shared_dist->pdf(node[gi]);
    if (std::isnan(w) || w < 0.0) w = 0.0;
    if (gi == 0 || gi == G - 1) w *= 0.5;          // trapezoidal endpoints
    weight[gi] = w;
    wsum += w;
  }
  if (!(wsum > 0.0)) return std::nullopt;

  double m1 = 0.0, m2 = 0.0;
  for (int gi = 0; gi < G; ++gi) {
    const double b = node[gi];
    const double w = weight[gi] / wsum;
    double mean_b = 0.0;    // E[C | b] = sum v_i q_i
    double var_b  = 0.0;    // Var[C | b] = sum v_i^2 q_i (1 - q_i)  (indep given b)
    for (const auto &t : terms) {
      const double c = evalSharedSide(gc, t.shared_side, shared, b);
      if (std::isnan(c)) return std::nullopt;
      auto dist = t.family->factory(t.p1, t.p2);
      double F = dist->cdf(c);
      if (std::isnan(F)) return std::nullopt;
      if (F < 0.0) F = 0.0; else if (F > 1.0) F = 1.0;
      double q;                                   // Pr[eps op c]
      switch (t.op) {
        case ComparisonOperator::LT:
        case ComparisonOperator::LE: q = F;        break;
        case ComparisonOperator::GT:
        case ComparisonOperator::GE: q = 1.0 - F;  break;
        default: return std::nullopt;
      }
      q *= t.bern;
      if (q < 0.0) q = 0.0; else if (q > 1.0) q = 1.0;
      mean_b += t.value * q;
      var_b  += t.value * t.value * q * (1.0 - q);
    }
    m1 += w * mean_b;
    m2 += w * (var_b + mean_b * mean_b);           // E[C^2|b] = Var + mean^2
  }
  return (k == 1) ? m1 : m2;
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

}  // extern "C"
