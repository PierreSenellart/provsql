/**
 * @file semiring/Formula.h
 * @brief Symbolic formula semiring producing readable provenance expressions.
 *
 * The @c Formula semiring (@c std::string, @f$\oplus@f$, @f$\otimes@f$,
 * "𝟘", "𝟙") represents provenance as a human-readable symbolic
 * expression using Unicode semiring symbols.  It is primarily used for
 * debugging and testing.
 *
 * Each gate evaluates to a string:
 * - @c zero()   → "𝟘"
 * - @c one()    → "𝟙"
 * - @c plus()   → "(a ⊕ b ⊕ …)" or just "a" for singletons
 * - @c times()  → "(a ⊗ b ⊗ …)" or just "a" for singletons
 * - @c monus()  → "(a ⊖ b)"
 * - @c delta()  → "δ(a)" or "δa" if @c a starts with @c (
 * - @c cmp()    → "[s1 op s2]"
 * - @c semimod()→ "x*s"
 * - @c agg()    → operator-specific notation (e.g., "min(a,b)")
 * - @c value()  → the literal string itself
 */
#ifndef FORMULA_H
#define FORMULA_H

#include <numeric>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>

#include "Semiring.h"

/**
 * @brief Concatenate elements of a range with a delimiter.
 *
 * Used internally by @c Formula::plus(), @c Formula::times(), and
 * @c Formula::agg() to build operator-separated strings.
 *
 * @tparam Range   Any range type with a @c value_type typedef.
 * @tparam Value   Element type (defaults to @c Range::value_type).
 * @param elements  The range to join.
 * @param delimiter String to insert between adjacent elements.
 * @return          All elements concatenated with @p delimiter between them.
 */
template <typename Range, typename Value = typename Range::value_type>
static std::string join(Range const& elements, const char *const delimiter) {
  std::ostringstream os;
  auto b = begin(elements), e = end(elements);

  if (b != e) {
    std::copy(b, prev(e), std::ostream_iterator<Value>(os, delimiter));
    b = prev(e);
  }
  if (b != e) {
    os << *b;
  }

  return os.str();
}

namespace semiring {
/**
 * @brief Symbolic provenance formula semiring over @c std::string.
 *
 * Evaluates circuits to human-readable Unicode provenance formulas.
 * Supports all optional operations (@c cmp, @c semimod, @c agg,
 * @c value) in addition to the mandatory ones.
 */
class Formula : public semiring::Semiring<std::string>
{
public:
virtual value_type zero() const override {
  return "𝟘";
}
virtual value_type one() const override {
  return "𝟙";
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  if(v.size()==0)
    return zero();
  else if(v.size()==1)
    return v[0];
  else
    return "("+join(v, " ⊕ ")+")";
}
virtual value_type times(const std::vector<value_type> &v) const override {
  if(v.size()==0)
    return one();
  else if(v.size()==1)
    return v[0];
  else
    return "("+join(v, " ⊗ ")+")";
}
virtual value_type monus(value_type x, value_type y) const override
{
  return "("+x+" ⊖ "+y+")";
}
virtual value_type delta(value_type x) const override
{
  if(x[0]=='(')
    return "δ"+x;
  else
    return "δ("+x+")";
}
virtual value_type cmp(value_type s1, ComparisonOperator op, value_type s2) const override {
  std::string result = "["+s1+" ";
  switch(op) {
  case ComparisonOperator::EQ:
    result+="=";
    break;
  case ComparisonOperator::NE:
    result+="≠";
    break;
  case ComparisonOperator::LE:
    result+="≤";
    break;
  case ComparisonOperator::LT:
    result+="<";
    break;
  case ComparisonOperator::GE:
    result+="≥";
    break;
  case ComparisonOperator::GT:
    result+=">";
    break;
  }
  return result+" "+s2+"]";
}
virtual value_type semimod(value_type x, value_type s) const override {
  return x + "*" + s;
}
virtual value_type agg(AggregationOperator op, const std::vector<std::string> &s) override {
  if(op==AggregationOperator::NONE)
    return "<>";

  if(s.empty()) {
    switch(op) {
    case AggregationOperator::COUNT:
    case AggregationOperator::SUM:
      return "0";
    case AggregationOperator::MIN:
      return "+∞";
    case AggregationOperator::MAX:
      return "-∞";
    case AggregationOperator::CHOOSE:
    case AggregationOperator::AVG:
      return "<>";
    case AggregationOperator::AND:
      return "⊤";
    case AggregationOperator::OR:
      return "⊥";
    case AggregationOperator::ARRAY_AGG:
      return "[]";
    case AggregationOperator::NONE:
      assert(false);
    }
  }

  std::string result;
  switch(op) {
  case AggregationOperator::ARRAY_AGG:
    result+="[";
    break;
  case AggregationOperator::MIN:
    result+="min(";
    break;
  case AggregationOperator::MAX:
    result+="max(";
    break;
  case AggregationOperator::AVG:
    result+="avg(";
    break;
  case AggregationOperator::CHOOSE:
    result+="choose(";
    break;
  default:
    ;
  }

  result += s[0];

  for(size_t i = 1; i<s.size(); ++i) {
    switch(op) {
    case AggregationOperator::COUNT:
    case AggregationOperator::SUM:
      result+="+";
      break;
    case AggregationOperator::MIN:
    case AggregationOperator::MAX:
    case AggregationOperator::AVG:
    case AggregationOperator::CHOOSE:
    case AggregationOperator::ARRAY_AGG:
      result+=",";
      break;
    case AggregationOperator::OR:
      result+="∨";
      break;
    case AggregationOperator::AND:
      result+="∧";
      break;
    case AggregationOperator::NONE:
      assert(false);
    }
    result+=s[i];
  }
  if(op==AggregationOperator::ARRAY_AGG)
    result+="]";
  else if(op==AggregationOperator::MIN ||
          op==AggregationOperator::MAX ||
          op==AggregationOperator::CHOOSE ||
          op==AggregationOperator::AVG)
    result+=")";
  return result;
}
virtual value_type value(const std::string &s) const override {
  return s;
}
};
}

#endif /* FORMULA_H */
