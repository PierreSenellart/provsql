/**
 * @file semiring/Formula.h
 * @brief Symbolic representation of provenance as a human-readable formula.
 *
 * The @c Formula pseudo-semiring (@c std::string, @f$\oplus@f$, @f$\otimes@f$,
 * "𝟘", "𝟙") produces a symbolic representation of provenance using
 * Unicode semiring symbols.  It is primarily used for debugging and
 * testing.
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

/**
 * @brief If @p s is wrapped in a single matched outer paren pair AND its
 *        top-level operator (depth 1, inside that pair) is @p op, return
 *        the inner content; otherwise return @p s unchanged.
 *
 * Used by @c Formula::plus() and @c Formula::times() to flatten same-op
 * nested gates by associativity: a child @c "(a ⊕ b)" feeding into a
 * parent @c plus is unwrapped to @c "a ⊕ b" so the join produces
 * @c "a ⊕ b ⊕ c" instead of @c "(a ⊕ b) ⊕ c". A different top-level op
 * (e.g., a @c times child) keeps its parens.
 */
static std::string strip_wrap_if_op(const std::string &s, const std::string &op) {
  if(s.size() < 2 || s.front() != '(' || s.back() != ')')
    return s;
  // Verify the leading '(' closes only at the very end : if any earlier
  // ')' brings depth back to 0, the outer pair isn't a single matched
  // wrap (e.g., @c "(a) ⊕ (b)" must not be stripped).
  int depth = 0;
  for(size_t i = 0; i < s.size() - 1; ++i) {
    if(s[i] == '(') ++depth;
    else if(s[i] == ')') {
      if(--depth == 0)
        return s;
    }
  }
  // Scan inner for a depth-0 occurrence of @p op. UTF-8 operators are
  // multi-byte but @c compare on raw bytes is correct since we never
  // straddle a UTF-8 char boundary at depth-0 positions outside parens.
  const std::string inner = s.substr(1, s.size() - 2);
  depth = 0;
  for(size_t i = 0; i + op.size() <= inner.size(); ) {
    if(inner[i] == '(') { ++depth; ++i; }
    else if(inner[i] == ')') { --depth; ++i; }
    else if(depth == 0 && inner.compare(i, op.size(), op) == 0)
      return inner;
    else
      ++i;
  }
  return s;
}

namespace semiring {
/**
 * @brief Symbolic provenance representation over @c std::string.
 *
 * Evaluates circuits to human-readable Unicode formulas.
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
  // Flatten same-op nesting by associativity: a child "(a ⊕ b)" is
  // inlined as "a ⊕ b" so the join produces "a ⊕ b ⊕ c", not
  // "(a ⊕ b) ⊕ c". Mixed-op children (e.g., a times subexpression)
  // keep their parens.
  std::vector<value_type> flat;
  flat.reserve(v.size());
  for(const auto &x : v)
    flat.push_back(strip_wrap_if_op(x, "⊕"));
  return "("+join(flat, " ⊕ ")+")";
}
virtual value_type times(const std::vector<value_type> &v) const override {
  if(v.size()==0)
    return one();
  else if(v.size()==1)
    return v[0];
  std::vector<value_type> flat;
  flat.reserve(v.size());
  for(const auto &x : v)
    flat.push_back(strip_wrap_if_op(x, "⊗"));
  return "("+join(flat, " ⊗ ")+")";
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
value_type parse_leaf(const char *v) const {
  return std::string(v);
}
/**
 * @brief Serialise a Formula evaluation as text.
 *
 * Drops the cosmetic outer paren pair that @c plus / @c times / @c monus
 * always produce: at the root there is no enclosing context, so the
 * outer parens carry no disambiguation value.
 */
std::string to_text(const value_type &s) const {
  if(s.size() < 2 || s.front() != '(' || s.back() != ')')
    return s;
  int depth = 0;
  for(size_t i = 0; i < s.size() - 1; ++i) {
    if(s[i] == '(') ++depth;
    else if(s[i] == ')') {
      if(--depth == 0)
        return s;
    }
  }
  return s.substr(1, s.size() - 2);
}
};
}

#endif /* FORMULA_H */
