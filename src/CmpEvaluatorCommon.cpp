/**
 * @file CmpEvaluatorCommon.cpp
 * @brief Implementation of the shared HAVING @c gate_cmp evaluator
 *        machinery.  See @c CmpEvaluatorCommon.h.
 */
#include "CmpEvaluatorCommon.h"

#include <algorithm>

#include "having_semantics.hpp" // extract_constant_string, semimod_extract_string_and_K, map_cmp_op, flip_op
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

  /* Identify the aggregate side; the other is the threshold constant.
   * The reversed order (const compared to agg) calls for op flipping.
   * The aggregate side is s * agg + d, s = ±1 and d a sum of constants,
   * once its constant arithmetic is peeled. */
  gate_t agg_side, const_side;
  std::vector<gate_t> via;
  std::vector<std::pair<int, std::string>> offsets;  // d as signed constants
  int sign = 1;
  auto peel = [&](gate_t g) -> gate_t {
    while (gc.getGateType(g) == gate_arith) {
      const auto &w = gc.getWires(g);
      const unsigned aop = static_cast<unsigned>(gc.getInfos(g).first);
      gate_t inner{};
      if (aop == PROVSQL_ARITH_PLUS) {
        int non_const = 0;
        for (gate_t ch : w)
          if (gc.getGateType(ch) != gate_value) { inner = ch; ++non_const; }
        if (non_const != 1) return g;
        for (gate_t ch : w)
          if (ch != inner) offsets.emplace_back(sign, gc.getExtra(ch));
      } else if (aop == PROVSQL_ARITH_MINUS && w.size() == 2) {
        if (gc.getGateType(w[1]) == gate_value) {          // X - c
          inner = w[0];
          offsets.emplace_back(-sign, gc.getExtra(w[1]));
        } else if (gc.getGateType(w[0]) == gate_value) {   // c - X
          inner = w[1];
          offsets.emplace_back(sign, gc.getExtra(w[0]));
          sign = -sign;
        } else
          return g;
      } else if (aop == PROVSQL_ARITH_NEG && w.size() == 1) {
        inner = w[0];
        sign = -sign;
      } else
        return g;
      via.push_back(g);
      g = inner;
    }
    return g;
  };
  agg_side = peel(cw[0]);
  const_side = cw[1];
  if (gc.getGateType(agg_side) != gate_agg) {
    via.clear(); offsets.clear(); sign = 1;
    agg_side = peel(cw[1]);
    const_side = cw[0];
    if (gc.getGateType(agg_side) != gate_agg) return false;
    op = provsql_having_detail::flip_op(op);
  }

  /* The comparison domain is the aggregate result type (info2 of the
   * gate_agg).  The closed-form evaluators handle the ordered numeric
   * domains (int / numeric / float) by scaling every value and the
   * threshold to a common integer grid from their decimal text -- so a
   * numeric(p,d) or finite-decimal float column is exact and fractional
   * thresholds work.  Text aggregates, exponential / non-decimal values,
   * and grids too wide for a @c long are declined here and left to the
   * enumeration path. */
  const unsigned aggtype =
    gc.getInfos(agg_side).second & PROVSQL_AGG_TYPE_MASK;  // strip scalar flag
  if (provsql_having_detail::aggtype_is_text(aggtype)) return false;

  std::string c_str;
  if (!provsql_having_detail::extract_constant_string(gc, const_side, c_str))
    return false;
  long c_mant = 0; int c_scale = 0;
  if (!provsql_having_detail::parse_decimal_scaled(c_str, c_mant, c_scale))
    return false;

  /* No child: only a scalar aggregation over no row, whose value is that of
   * the empty input (a window frame that may be empty while its row exists);
   * a group without rows is no gate_agg. */
  const auto &agg_children = gc.getWires(agg_side);
  if (agg_children.empty() &&
      !(gc.getInfos(agg_side).second & PROVSQL_AGG_SCALAR_FLAG))
    return false;

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

  /* The gate records the aggregate the query wrote: count(*) and count(expr)
   * both keep COUNT even though their contributions are 1 and 0/1, so there is
   * nothing to infer from the values here. */
  AggregationOperator agg_kind =
    getAggregationOperator(gc.getInfos(agg_side).first);

  std::vector<long> d_mant(offsets.size());
  std::vector<int> d_scale(offsets.size());
  for (std::size_t i = 0; i < offsets.size(); ++i)
    if (!provsql_having_detail::parse_decimal_scaled(offsets[i].second,
                                                     d_mant[i], d_scale[i]))
      return false;

  /* Rescale every value, the threshold and the offsets to a common integer
   * grid. */
  int target = c_scale;
  for (int s : m_scale) target = std::max(target, s);
  for (int s : d_scale) target = std::max(target, s);
  long C = 0;
  if (!provsql_having_detail::rescale_to(c_mant, c_scale, target, C)) return false;
  std::vector<long> ms(m_mant.size());
  for (std::size_t i = 0; i < m_mant.size(); ++i)
    if (!provsql_having_detail::rescale_to(m_mant[i], m_scale[i], target, ms[i]))
      return false;

  /* s * agg + d  op  C  is  agg op C - d  (s = 1), or agg flip(op) d - C. */
  long d = 0;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    long v = 0;
    if (!provsql_having_detail::rescale_to(d_mant[i], d_scale[i], target, v))
      return false;
    d += offsets[i].first * v;
  }
  if (sign > 0)
    C -= d;
  else {
    C = d - C;
    op = provsql_having_detail::flip_op(op);
  }

  out.via = std::move(via);
  out.agg = agg_side;
  out.semimods = std::move(semimods);
  out.ks = std::move(ks);
  out.ms = std::move(ms);
  out.agg_kind = agg_kind;
  out.op = op;
  out.C = C;
  return true;
}

bool aggPrivateToCmp(const AggCmpMatch &match, const std::vector<unsigned> &ref)
{
  if (ref[static_cast<std::size_t>(match.agg)] != 1)
    return false;
  for (gate_t g : match.via)
    if (ref[static_cast<std::size_t>(g)] != 1)
      return false;
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
