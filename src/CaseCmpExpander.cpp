/**
 * @file CaseCmpExpander.cpp
 * @brief Implementation of @c runCaseCmpExpander.  See @c CaseCmpExpander.h.
 */
#include "CaseCmpExpander.h"

#include <string>
#include <vector>

#include "having_semantics.hpp"  // extract_constant_string, map_cmp_op, parse_decimal_scaled

extern "C" {
#include "provsql_utils.h"  // gate_type enum
}

namespace provsql {

namespace {

/// Whether @p op holds of a pair whose comparison has sign @p sign.
bool op_holds(ComparisonOperator op, int sign)
{
  switch (op) {
  case ComparisonOperator::EQ: return sign == 0;
  case ComparisonOperator::NE: return sign != 0;
  case ComparisonOperator::LT: return sign < 0;
  case ComparisonOperator::LE: return sign <= 0;
  case ComparisonOperator::GT: return sign > 0;
  case ComparisonOperator::GE: return sign >= 0;
  }
  return false;
}

/**
 * @brief The constant an operand carries, where it carries one.
 *
 * An arm of a lowered @c CASE is a bare @c gate_value; the other side of a
 * comparison the rewriter builds is the @c semimod(𝟙, value) of a value of
 * the row.  @p is_null says the constant is SQL's NULL, which no comparison
 * holds of.
 */
bool constant_of(GenericCircuit &gc, gate_t g, std::string &out, bool &is_null)
{
  const gate_type t = gc.getGateType(g);

  if (t == gate_value)
    out = gc.getExtra(g);
  else if (t == gate_semimod) {
    if (!provsql_having_detail::extract_constant_string(gc, g, out))
      return false;
  } else
    return false;

  is_null = out.empty() || out == "NULL" ||
            gc.getUUID(g) == provsql_having_detail::GATE_NULL_UUID;
  return true;
}

/// Decide a comparison between two constants, if both are decimal numbers.
bool decide_constant_cmp(GenericCircuit &gc, gate_t l, gate_t r,
                         ComparisonOperator op, bool &holds)
{
  std::string ls, rs;
  bool lnull = false, rnull = false;
  long lm = 0, rm = 0;
  int lscale = 0, rscale = 0, scale;

  if (!constant_of(gc, l, ls, lnull) || !constant_of(gc, r, rs, rnull))
    return false;
  if (lnull || rnull) {
    holds = false;   /* unknown, and an unknown guard does not hold */
    return true;
  }
  if (!provsql_having_detail::parse_decimal_scaled(ls, lm, lscale) ||
      !provsql_having_detail::parse_decimal_scaled(rs, rm, rscale))
    return false;
  scale = lscale > rscale ? lscale : rscale;
  if (!provsql_having_detail::rescale_to(lm, lscale, scale, lm) ||
      !provsql_having_detail::rescale_to(rm, rscale, scale, rm))
    return false;
  holds = op_holds(op, lm < rm ? -1 : (lm > rm ? 1 : 0));
  return true;
}

/**
 * @brief Expand one comparison whose operand at @p side is a @c gate_case.
 *
 * The cmp gate itself becomes the @c gate_plus of the arm terms, keeping its
 * id (and so every parent that reads it).
 */
void expand(GenericCircuit &gc, gate_t cmp, unsigned side, gate_t one)
{
  /* Copy before creating gates: the wire vectors may move. */
  const std::vector<gate_t> cw = gc.getWires(cmp);
  const gate_t selection = cw[side], other = cw[1 - side];
  const std::vector<gate_t> aw = gc.getWires(selection);
  const std::pair<unsigned, unsigned> infos = gc.getInfos(cmp);
  const std::size_t n = aw.size(), arms = (n - 1) / 2;
  bool okop = false;
  ComparisonOperator op = provsql_having_detail::map_cmp_op(gc, cmp, okop);
  std::vector<gate_t> terms;

  for (std::size_t i = 0; i <= arms; ++i) {
    const bool is_default = (i == arms);
    const gate_t value = is_default ? aw[n - 1] : aw[2 * i + 1];
    const gate_t l = (side == 0) ? value : other;
    const gate_t r = (side == 0) ? other : value;
    std::vector<gate_t> factors;
    gate_t arm_cmp;
    bool holds = false;

    /* The comparison of this arm, in the operand order of the original. */
    if (okop && decide_constant_cmp(gc, l, r, op, holds)) {
      if (!holds)
        continue;    /* the term is 𝟘: the arm never satisfies the comparison */
      arm_cmp = gc.addAnonymousGate(gate_one, {});
    } else {
      arm_cmp = gc.addAnonymousGate(gate_cmp, {l, r});
      gc.setInfos(arm_cmp, infos.first, infos.second);
    }

    /* The arm is selected where its guard holds and none before it does. */
    for (std::size_t j = 0; j < i; ++j)
      factors.push_back(gc.addAnonymousGate(gate_monus, {one, aw[2 * j]}));
    if (!is_default)
      factors.push_back(aw[2 * i]);
    factors.push_back(arm_cmp);
    terms.push_back(factors.size() == 1
                      ? factors[0]
                      : gc.addAnonymousGate(gate_times, std::move(factors)));
  }

  if (terms.empty())
    gc.resolveGateToZero(cmp);   /* no arm ever satisfies it */
  else
    gc.resolveToPlus(cmp, std::move(terms));
}

}  // namespace

unsigned runCaseCmpExpander(GenericCircuit &gc)
{
  unsigned expanded = 0;
  gate_t one{};
  bool have_one = false;

  /* A comparison between two guarded selections takes one round per side, a
   * nested selection one per level; the bound is a guard against a circuit
   * that would build them without end. */
  for (unsigned round = 0; round < 32; ++round) {
    std::vector<std::pair<gate_t, unsigned>> todo;
    const auto nb = gc.getNbGates();

    for (std::size_t i = 0; i < nb; ++i) {
      const auto g = static_cast<gate_t>(i);
      if (gc.getGateType(g) != gate_cmp)
        continue;
      const auto &w = gc.getWires(g);
      if (w.size() != 2)
        continue;
      /* The wires of a gate_case are (guard, value)* followed by the
       * default, so an odd number of them, at least one. */
      for (unsigned side = 0; side < 2; ++side)
        if (gc.getGateType(w[side]) == gate_case) {
          const std::size_t n = gc.getWires(w[side]).size();
          if (n >= 1 && n % 2 == 1)
            todo.emplace_back(g, side);
          break;
        }
    }
    if (todo.empty())
      break;

    if (!have_one) {
      one = gc.addAnonymousGate(gate_one, {});
      have_one = true;
    }
    for (const auto &t : todo) {
      if (gc.getGateType(t.first) != gate_cmp)
        continue;   /* defensive: already rewritten */
      expand(gc, t.first, t.second, one);
      ++expanded;
    }
  }
  return expanded;
}

}  // namespace provsql
