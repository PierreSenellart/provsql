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
 * - @c unmapped_input() → the leaf's abbreviated UUID ("1361b50e…")
 *
 * It also renders the measure-carrier gates that carry no algebraic
 * meaning, and that every proper semiring therefore refuses -- being a
 * serialisation of the circuit rather than an evaluation of it,
 * @c Formula has a faithful rendering for each and refuses nothing:
 * - @c rv()          → "normal(2.5, 0.5)" (wired parameters substituted)
 * - @c arith()       → ordinary arithmetic notation ("(a + b)", "ln(a)"…),
 *                      kept visually distinct from the semiring's
 *                      @f$\oplus@f$ / @f$\otimes@f$
 * - @c mixture()     → "(p ? x : y)"
 * - @c categorical() → "categorical(κ; 0.3: a, 0.7: b)"
 * - @c guarded_case()→ "case(g → v; else d)"
 * - @c observe()     → "observe(x = 2.5)"
 * - @c conditioned() → "(x | c)"
 */
#ifndef FORMULA_H
#define FORMULA_H

#include <numeric>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
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

/**
 * @brief Render a probability for display in a symbolic formula.
 *
 * Enough significant digits that the usual decimal probabilities print
 * back as themselves (@c 0.3, not @c 0.299999), without the full
 * round-trip verbosity of @c setprecision(17).
 */
static std::string format_number(double v) {
  std::ostringstream os;
  os << std::setprecision(15) << v;
  return os.str();
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
/**
 * @brief Render a random-variable leaf from its on-disk encoding.
 *
 * @c "normal:2.5,0.5" becomes @c "normal(2.5, 0.5)"; a wired parameter
 * (written @c "$i", making the leaf a compound / latent one) is replaced
 * by the rendering of the corresponding wire.  Parsing here is
 * deliberately textual: the rendering must survive any family the
 * distribution registry gains, including one this build does not know.
 */
virtual value_type rv(const std::string &spec,
                      const std::vector<value_type> &params) const override {
  const auto colon = spec.find(':');
  if(colon == std::string::npos)
    return spec;

  std::vector<value_type> args;
  size_t pos = colon + 1;
  while(pos <= spec.size()) {
    const auto comma = spec.find(',', pos);
    const auto end = (comma == std::string::npos ? spec.size() : comma);
    std::string arg = spec.substr(pos, end - pos);
    if(arg.size() > 1 && arg[0] == '$') {
      try {
        const size_t slot = std::stoul(arg.substr(1));
        if(slot < params.size())
          arg = params[slot];
      } catch(const std::exception &) {
        // Malformed wire reference: keep the raw text.
      }
    }
    args.push_back(arg);
    if(comma == std::string::npos)
      break;
    pos = comma + 1;
  }

  return spec.substr(0, colon) + "(" + join(args, ", ") + ")";
}
/**
 * @brief Render an arithmetic gate in ordinary arithmetic notation.
 *
 * Deliberately ASCII (@c +, @c *, @c -, @c /, @c ^) so that arithmetic
 * over scalar children stays visually distinct from the semiring's
 * @f$\oplus@f$ / @f$\otimes@f$ / @f$\ominus@f$.  An operand count that
 * does not match the operator's arity falls back to a functional
 * rendering rather than misrepresenting the circuit.
 */
virtual value_type arith(ArithmeticOperator op,
                         const std::vector<value_type> &v,
                         const std::string &extra) const override {
  const auto infix = [&v](const char *sep) {
                       return "(" + join(v, sep) + ")";
                     };
  const auto functional = [&v](const char *name) {
                            return name + ("(" + join(v, ", ") + ")");
                          };

  switch(op) {
  case ArithmeticOperator::PLUS:
    if(v.empty()) return "0";
    return v.size() == 1 ? v[0] : infix(" + ");
  case ArithmeticOperator::TIMES:
    if(v.empty()) return "1";
    return v.size() == 1 ? v[0] : infix(" * ");
  case ArithmeticOperator::MINUS:
    return v.size() == 2 ? infix(" - ") : functional("minus");
  case ArithmeticOperator::DIV:
    return v.size() == 2 ? infix(" / ") : functional("div");
  case ArithmeticOperator::NEG:
    return v.size() == 1 ? "(-" + v[0] + ")" : functional("neg");
  case ArithmeticOperator::MAX:
    return functional("max");
  case ArithmeticOperator::MIN:
    return functional("min");
  case ArithmeticOperator::POW:
    return v.size() == 2 ? infix(" ^ ") : functional("pow");
  case ArithmeticOperator::LN:
    return functional("ln");
  case ArithmeticOperator::EXP:
    return functional("exp");
  case ArithmeticOperator::PERCENTILE:
    // Interleaved [indicator, value] wires; the fraction is in extra.
    if(v.empty() || v.size() % 2 != 0)
      return functional("percentile");
    {
      std::vector<value_type> rows;
      for(size_t i = 0; i + 1 < v.size(); i += 2)
        rows.push_back("[" + v[i] + "] " + v[i+1]);
      return "percentile(" + extra + "; " + join(rows, ", ") + ")";
    }
  }
  return functional("arith");
}
/** @brief Render a Bernoulli mixture as a conditional expression. */
virtual value_type mixture(value_type p, value_type x, value_type y)
const override {
  return "(" + p + " ? " + x + " : " + y + ")";
}
/**
 * @brief Render a categorical mixture as its list of weighted outcomes,
 *        prefixed by the key that ties them to a single draw (two
 *        categoricals sharing a key share the draw).
 */
virtual value_type categorical(value_type key,
                               const std::vector<double> &probs,
                               const std::vector<std::string> &outcomes)
const override {
  std::vector<value_type> arms;
  for(size_t i = 0; i < probs.size() && i < outcomes.size(); ++i)
    arms.push_back(format_number(probs[i]) + ": " + outcomes[i]);
  return "categorical(" + key + "; " + join(arms, ", ") + ")";
}
/** @brief Render a guarded selection, first-match order preserved. */
virtual value_type guarded_case(const std::vector<value_type> &v)
const override {
  if(v.empty() || v.size() % 2 == 0)
    return "case(" + join(v, ", ") + ")";

  std::vector<value_type> arms;
  for(size_t i = 0; i + 1 < v.size(); i += 2)
    arms.push_back(v[i] + " → " + v[i+1]);
  arms.push_back("else " + v.back());
  return "case(" + join(arms, "; ") + ")";
}
/** @brief Render a likelihood-weighting observation. */
virtual value_type observe(value_type child, const std::string &datum)
const override {
  return "observe(" + child + " = " + datum + ")";
}
/**
 * @brief Render a conditioning marker as @c "(target | evidence)".
 *
 * A three-wire (Boolean-event) conditioned gate also carries the
 * materialised joint @c times(target, evidence) as its third wire; it
 * is redundant with the first two and left out of the rendering.
 */
virtual value_type conditioned(const std::vector<value_type> &v)
const override {
  if(v.size() < 2)
    return "cond(" + join(v, ", ") + ")";
  return "(" + v[0] + " | " + v[1] + ")";
}
/**
 * @brief Identify a variable leaf the provenance mapping does not name
 *        by an abbreviated form of its UUID.
 *
 * @c Formula serialises the circuit, so the base class's
 * @f$\mathbb{1}@f$ would be doubly wrong here: it makes every unnamed
 * leaf look alike, and -- being the multiplicative identity -- it is
 * dropped by the enclosing @c times(), collapsing a whole join to
 * @c "𝟙".  An abbreviated UUID keeps the structure and stays
 * recognisable against what ProvSQL Studio prints on the circuit's
 * nodes.  The first UUID group is kept: 32 bits, enough to tell the
 * leaves of one circuit apart at a glance, with the full value one
 * @c get_children / Studio click away.
 */
virtual value_type unmapped_input(const std::string &uuid) const override {
  return uuid.size() > 8 ? uuid.substr(0, 8) + "…" : uuid;
}
value_type parse_leaf(const char *v) const {
  return std::string(v);
}
/**
 * @brief Special case: @c Formula serialises the circuit structure as
 *        a string rather than computing a semantic value, so a
 *        safe-query-rewritten circuit renders to its (rewritten)
 *        structural formula and remains a faithful description.  The
 *        homomorphism question does not arise.
 */
virtual bool compatibleWithBooleanRewrite() const override {
  return true;
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
