/**
 * @file semiring/Regex.h
 * @brief Regular-expression semiring: the derivations of a tuple as a
 *        language over the labels of its input tuples.
 *
 * The semiring of regular languages over an alphabet of labels, with
 * @f$\oplus@f$ union, @f$\otimes@f$ concatenation, @f$\mathbb{0} =
 * \emptyset@f$ and @f$\mathbb{1} = \{\varepsilon\}@f$, each language
 * written as a regular expression.  Concatenation does not commute: a
 * product gate concatenates its factors in the order of the query, that of
 * the @c FROM clause for a join, so that a path query reads its paths in
 * the order it extends them.
 *
 * Every value has a star, the Kleene star, which makes the regular
 * expression of a recursion on cyclic data its exact least solution: the
 * equations are solved by Gaussian elimination (state elimination, in
 * automata terms), provided the recursive relation is on the same side of
 * every coefficient (see @c GenericCircuit::solveFixSystem).
 *
 * Operations:
 * - @c zero()   → @f$\emptyset@f$
 * - @c one()    → @f$\varepsilon@f$
 * - @c plus()   → @c "a | b", alternatives that render alike merged
 * - @c times()  → @c "a · b"
 * - @c star()   → @c "(a)*"
 *
 * Equality is syntactic: two expressions of the same language may compare
 * unequal, which only leaves an alternative unmerged.
 */
#ifndef REGEX_H
#define REGEX_H

#include <string>
#include <vector>

#include "Semiring.h"

namespace semiring {

/**
 * @brief A regular expression, with its rendering and the operator at its
 *        root.
 *
 * A union keeps its alternatives apart, so that a union of unions is
 * flattened and an alternative already present is not repeated.
 */
struct regex_t {
  /** @brief The operator at the root of the expression. */
  enum Kind : unsigned char { EMPTY, EPSILON, ATOM, STAR, CONCAT, UNION };
  Kind kind = EMPTY;              ///< The operator at the root
  std::string text;               ///< Rendering, for every kind but UNION
  std::vector<std::string> alts;  ///< Alternatives of a UNION, rendered

  /** @brief Syntactic equality. */
  bool operator==(const regex_t &o) const {
    return kind == o.kind && text == o.text && alts == o.alts;
  }
  /** @brief Syntactic inequality. */
  bool operator!=(const regex_t &o) const {
    return !(*this == o);
  }

  /** @brief The expression, as text. */
  std::string render() const {
    switch(kind) {
    case EMPTY: return "∅";
    case EPSILON: return "ε";
    case UNION: {
      std::string r;
      for(const auto &a : alts)
        r += (r.empty() ? "" : " | ") + a;
      return r;
    }
    default: return text;
    }
  }

  /** @brief The expression, as an operand of a concatenation. */
  std::string as_factor() const {
    return kind == UNION ? "(" + render() + ")" : render();
  }
};

/**
 * @brief Operations on regular expressions, shared by @c Regex and
 *        @c TropicalPaths.
 */
struct regex_ops {
  /** @brief A one-letter language. */
  static regex_t atom(const std::string &label) {
    regex_t r;
    if(label.empty())
      r.kind = regex_t::EPSILON;
    else {
      r.kind = regex_t::ATOM;
      r.text = label;
    }
    return r;
  }

  /** @brief The union of @p v. */
  static regex_t plus(const std::vector<regex_t> &v) {
    regex_t r;
    bool epsilon = false, starred = false;
    for(const auto &x : v) {
      std::vector<std::string> add;
      switch(x.kind) {
      case regex_t::EMPTY:
        continue;
      case regex_t::EPSILON:
        epsilon = true;
        continue;
      case regex_t::UNION:
        add = x.alts;
        break;
      case regex_t::STAR:
        starred = true;
        add.push_back(x.text);
        break;
      default:
        add.push_back(x.text);
      }
      for(const auto &a : add) {
        bool seen = false;
        for(const auto &b : r.alts)
          seen = seen || a == b;
        if(!seen)
          r.alts.push_back(a);
      }
    }
    if(r.alts.empty()) {
      r.kind = epsilon ? regex_t::EPSILON : regex_t::EMPTY;
      return r;
    }
    /* ε is in every starred expression already. */
    if(epsilon && !starred)
      r.alts.insert(r.alts.begin(), "ε");
    /* A single alternative: the expression it came from (a union has
     * at least two). */
    if(r.alts.size() == 1)
      for(const auto &x : v)
        if(x.kind != regex_t::UNION && x.text == r.alts[0])
          return x;
    r.kind = regex_t::UNION;
    return r;
  }

  /** @brief The concatenation of @p v, in this order. */
  static regex_t times(const std::vector<regex_t> &v) {
    regex_t r;
    r.kind = regex_t::EPSILON;
    std::size_t factors = 0;
    const regex_t *last = nullptr;
    for(const auto &x : v) {
      if(x.kind == regex_t::EMPTY)
        return regex_t();
      if(x.kind == regex_t::EPSILON)
        continue;
      r.text += (factors ? " · " : "") + x.as_factor();
      ++factors;
      last = &x;
    }
    if(factors == 1)
      return *last;
    if(factors > 1)
      r.kind = regex_t::CONCAT;
    return r;
  }

  /** @brief The Kleene star of @p a. */
  static regex_t star(const regex_t &a) {
    switch(a.kind) {
    case regex_t::EMPTY:
    case regex_t::EPSILON:
      return atom("");
    case regex_t::STAR:
      return a;
    default:
      break;
    }
    regex_t b = a;
    if(b.kind == regex_t::UNION) {
      /* (ε | a)* = a* */
      std::vector<std::string> alts;
      for(const auto &x : b.alts)
        if(x != "ε")
          alts.push_back(x);
      b.alts = alts;
      if(b.alts.size() == 1) {
        b.kind = regex_t::CONCAT;
        b.text = b.alts[0];
        b.alts.clear();
      }
    }
    regex_t r;
    r.kind = regex_t::STAR;
    const std::string t = b.render();
    r.text = b.kind == regex_t::ATOM && single_code_point(t) ? t + "*"
                                                             : "(" + t + ")*";
    return r;
  }

  /** @brief Whether the UTF-8 string @p t is a single code point. */
  static bool single_code_point(const std::string &t) {
    if(t.empty())
      return false;
    for(std::size_t i = 1; i < t.size(); ++i)
      if(((unsigned char) t[i] & 0xC0) != 0x80)
        return false;
    return true;
  }
};

/**
 * @brief The regular-expression semiring over @c regex_t.
 *
 * Input tuples are read from the mapping as their labels, an empty label
 * standing for @f$\varepsilon@f$.
 */
class Regex : public semiring::Semiring<regex_t>
{
public:
virtual value_type zero() const override {
  return regex_t();
}
virtual value_type one() const override {
  return regex_ops::atom("");
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  return regex_ops::plus(v);
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return regex_ops::times(v);
}
virtual value_type monus(value_type, value_type) const override {
  throw SemiringException(
          "The regular-expression semiring has no monus: the difference "
          "of two languages is not, in general, a semiring difference.");
}
virtual value_type delta(value_type x) const override {
  return x.kind == regex_t::EMPTY ? zero() : one();
}
/** @brief Union is idempotent. */
virtual bool idempotent() const override {
  return true;
}
/** @brief Concatenation does not commute. */
virtual bool commutative() const override {
  return false;
}
/** @brief Every language has a Kleene star. */
virtual bool has_star() const override {
  return true;
}
virtual value_type star(const value_type &a) const override {
  return regex_ops::star(a);
}
/** @brief Equality is syntactic. */
virtual bool exact_equality() const override {
  return false;
}
value_type parse_leaf(const char *v) const {
  return regex_ops::atom(v);
}
/** @brief The expression, as text. */
std::string to_text(const value_type &v) const {
  return v.render();
}
};

}

#endif /* REGEX_H */
