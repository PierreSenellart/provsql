/**
 * @file CmpEvaluatorCommon.cpp
 * @brief Implementation of the shared HAVING @c gate_cmp evaluator
 *        machinery.  See @c CmpEvaluatorCommon.h.
 */
#include "CmpEvaluatorCommon.h"

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

  /* Identify which side is the gate_agg and which is the constant
   * wrapper.  Both orderings are legitimate (agg compared to const, or
   * const compared to agg); the second case calls for op flipping. */
  int C = 0;
  gate_t agg_side = cw[0], const_side = cw[1];
  if (gc.getGateType(agg_side) != gate_agg ||
      !provsql_having_detail::extract_constant_C(gc, const_side, C)) {
    agg_side = cw[1]; const_side = cw[0];
    if (gc.getGateType(agg_side) != gate_agg ||
        !provsql_having_detail::extract_constant_C(gc, const_side, C)) {
      return false;
    }
    op = provsql_having_detail::flip_op(op);
  }

  const auto &agg_children = gc.getWires(agg_side);
  if (agg_children.empty()) return false;

  std::vector<gate_t> semimods, ks;
  std::vector<int> ms;
  semimods.reserve(agg_children.size());
  ks.reserve(agg_children.size());
  ms.reserve(agg_children.size());

  for (gate_t ch : agg_children) {
    if (gc.getGateType(ch) != gate_semimod) return false;
    int m = 0;
    gate_t k_gate{};
    if (!provsql_having_detail::semimod_extract_M_and_K(gc, ch, m, k_gate))
      return false;
    semimods.push_back(ch);
    ks.push_back(k_gate);
    ms.push_back(m);
  }

  /* Effective aggregate kind.  COUNT(*) reaches the circuit as SUM of
   * unit-weighted semimods; mirror pw_from_cmp_gate's remap so callers
   * see COUNT rather than SUM in that case. */
  AggregationOperator agg_kind =
    getAggregationOperator(gc.getInfos(agg_side).first);
  if (agg_kind == AggregationOperator::SUM) {
    bool all_one = true;
    for (int m : ms) if (m != 1) { all_one = false; break; }
    if (all_one) agg_kind = AggregationOperator::COUNT;
  }

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
