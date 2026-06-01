/**
 * @file CmpEvaluatorCommon.cpp
 * @brief Implementation of the shared HAVING @c gate_cmp evaluator
 *        machinery.  See @c CmpEvaluatorCommon.h.
 */
#include "CmpEvaluatorCommon.h"

#include <algorithm>

#include "having_semantics.hpp" // extract_constant_C, semimod_extract_M_and_K, map_cmp_op, flip_op
extern "C" {
#include "provsql_utils.h"      // gate_type enum
}

namespace provsql {

bool matchAggCmp(GenericCircuit &gc, gate_t cmp, AggCmpMatch &out)
{
  const auto &cw = gc.getWires(cmp);
  if (cw.size() != 2) return false;

  bool okop = false;
  ComparisonOperator op = provsql_having_detail::map_cmp_op(gc, cmp, okop);
  if (!okop) return false;

  /* Identify the gate_agg side; the other is the threshold constant.
   * The reversed order (const compared to agg) calls for op flipping. */
  gate_t agg_side, const_side;
  if (gc.getGateType(cw[0]) == gate_agg) {
    agg_side = cw[0]; const_side = cw[1];
  } else if (gc.getGateType(cw[1]) == gate_agg) {
    agg_side = cw[1]; const_side = cw[0];
    op = provsql_having_detail::flip_op(op);
  } else {
    return false;
  }

  /* The comparison domain is the aggregate result type (info2 of the
   * gate_agg).  The closed-form evaluators handle the ordered numeric
   * domains (int / numeric / float) by scaling every value and the
   * threshold to a common integer grid from their decimal text -- so a
   * numeric(p,d) or finite-decimal float column is exact and fractional
   * thresholds work.  Text aggregates, exponential / non-decimal values,
   * and grids too wide for a @c long are declined here and left to the
   * enumeration path. */
  const unsigned aggtype = gc.getInfos(agg_side).second;
  if (provsql_having_detail::aggtype_is_text(aggtype)) return false;

  std::string c_str;
  if (!provsql_having_detail::extract_constant_string(gc, const_side, c_str))
    return false;
  long c_mant = 0; int c_scale = 0;
  if (!provsql_having_detail::parse_decimal_scaled(c_str, c_mant, c_scale))
    return false;

  const auto &agg_children = gc.getWires(agg_side);
  if (agg_children.empty()) return false;

  std::vector<gate_t> semimods, ks;
  std::vector<long> m_mant;
  std::vector<int> m_scale;
  semimods.reserve(agg_children.size());
  ks.reserve(agg_children.size());
  m_mant.reserve(agg_children.size());
  m_scale.reserve(agg_children.size());

  for (gate_t ch : agg_children) {
    if (gc.getGateType(ch) != gate_semimod) return false;
    std::string m_str;
    gate_t k_gate{};
    if (!provsql_having_detail::semimod_extract_string_and_K(gc, ch, m_str, k_gate))
      return false;
    long mm = 0; int sc = 0;
    if (!provsql_having_detail::parse_decimal_scaled(m_str, mm, sc))
      return false;
    semimods.push_back(ch);
    ks.push_back(k_gate);
    m_mant.push_back(mm);
    m_scale.push_back(sc);
  }

  /* Effective aggregate kind.  COUNT(*) reaches the circuit as SUM of
   * unit-weighted semimods (detected on the unscaled mantissas); mirror
   * pw_from_cmp_gate's remap so callers see COUNT in that case. */
  AggregationOperator agg_kind =
    getAggregationOperator(gc.getInfos(agg_side).first);
  if (agg_kind == AggregationOperator::SUM) {
    bool all_one = true;
    for (std::size_t i = 0; i < m_mant.size(); ++i)
      if (!(m_mant[i] == 1 && m_scale[i] == 0)) { all_one = false; break; }
    if (all_one) agg_kind = AggregationOperator::COUNT;
  }

  /* Rescale every value and the threshold to a common integer grid. */
  int target = c_scale;
  for (int s : m_scale) target = std::max(target, s);
  long C = 0;
  if (!provsql_having_detail::rescale_to(c_mant, c_scale, target, C)) return false;
  std::vector<long> ms(m_mant.size());
  for (std::size_t i = 0; i < m_mant.size(); ++i)
    if (!provsql_having_detail::rescale_to(m_mant[i], m_scale[i], target, ms[i]))
      return false;

  out.agg = agg_side;
  out.semimods = std::move(semimods);
  out.ks = std::move(ks);
  out.ms = std::move(ms);
  out.agg_kind = agg_kind;
  out.op = op;
  out.C = C;
  return true;
}

std::vector<unsigned> computeRefCounts(const GenericCircuit &gc)
{
  const auto nb = gc.getNbGates();
  std::vector<unsigned> ref(nb, 0);
  for (std::size_t i = 0; i < nb; ++i) {
    auto g = static_cast<gate_t>(i);
    for (gate_t w : gc.getWires(g)) {
      const auto idx = static_cast<std::size_t>(w);
      if (idx < ref.size()) ++ref[idx];
    }
  }
  return ref;
}

double contributorProb(const GenericCircuit &gc, gate_t g,
                       const std::vector<unsigned> &ref, bool &ok)
{
  switch (gc.getGateType(g)) {
    case gate_one:  return 1.0;
    case gate_zero: return 0.0;
    case gate_input:
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      return gc.getProb(g);
    case gate_times: {
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      double pr = 1.0;
      for (gate_t c : gc.getWires(g)) {
        pr *= contributorProb(gc, c, ref, ok);
        if (!ok) return 0.0;
      }
      return pr;
    }
    case gate_plus: {
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      double q = 1.0;
      for (gate_t c : gc.getWires(g)) {
        q *= (1.0 - contributorProb(gc, c, ref, ok));
        if (!ok) return 0.0;
      }
      return 1.0 - q;
    }
    case gate_monus: {
      /* a (-) b = a AND NOT b ; with disjoint private leaves a and b are
       * independent, so Pr = Pr(a) * (1 - Pr(b)).  Children are
       * [minuend, subtrahend] (see GenericCircuit evaluate<S>). */
      if (ref[static_cast<std::size_t>(g)] != 1) { ok = false; return 0.0; }
      const auto &w = gc.getWires(g);
      if (w.size() != 2) { ok = false; return 0.0; }
      double pa = contributorProb(gc, w[0], ref, ok);
      if (!ok) return 0.0;
      double pb = contributorProb(gc, w[1], ref, ok);
      if (!ok) return 0.0;
      return pa * (1.0 - pb);
    }
    default:
      ok = false;
      return 0.0;
  }
}

}  // namespace provsql
