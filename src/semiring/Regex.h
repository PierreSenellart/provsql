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

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Semiring.h"
#include "../Circuit.h"

namespace semiring {

struct regex_node;

/**
 * @brief A regular expression: an immutable tree, shared between the
 *        expressions built from it.
 *
 * Copying an expression, or concatenating two, copies no text: the text is
 * rendered only once, by @c render(), for a result.  Unions and
 * concatenations are kept flat, so that a union of unions is one union,
 * without an alternative already present.
 */
struct regex_t {
  /** @brief The operator at the root of the expression. */
  enum Kind : unsigned char { EMPTY, EPSILON, ATOM, STAR, CONCAT, UNION };
  std::shared_ptr<const regex_node> node; ///< The root; null for ∅

  inline Kind kind() const;
  inline std::size_t hash() const;
  /** @brief Syntactic equality. */
  inline bool operator==(const regex_t &o) const;
  /** @brief Syntactic inequality. */
  bool operator!=(const regex_t &o) const {
    return !(*this == o);
  }
  /** @brief The expression, as text. */
  std::string render() const {
    std::string out;
    render(out);
    return out;
  }
  /** @brief Append the expression, as text, to @p out. */
  inline void render(std::string &out) const;
};

/** @brief A node of a @c regex_t. */
struct regex_node {
  regex_t::Kind kind;          ///< Its operator
  std::string label;           ///< The label of an ATOM
  std::vector<regex_t> kids;   ///< The operands of STAR, CONCAT and UNION
  std::size_t hash;            ///< Of the whole expression
};

regex_t::Kind regex_t::kind() const {
  return node ? node->kind : EMPTY;
}

std::size_t regex_t::hash() const {
  return node ? node->hash : 0;
}

bool regex_t::operator==(const regex_t &o) const {
  if(node == o.node)
    return true;
  if(!node || !o.node || node->hash != o.node->hash ||
     node->kind != o.node->kind || node->label != o.node->label ||
     node->kids.size() != o.node->kids.size())
    return false;
  for(std::size_t i = 0; i < node->kids.size(); ++i)
    if(node->kids[i] != o.node->kids[i])
      return false;
  return true;
}

void regex_t::render(std::string &out) const {
  switch(kind()) {
  case EMPTY: out += "∅"; return;
  case EPSILON: out += "ε"; return;
  case ATOM: out += node->label; return;
  case UNION:
  case CONCAT: {
    const char *sep = kind() == UNION ? " | " : " · ";
    bool first = true;
    for(const auto &k : node->kids) {
      if(!first)
        out += sep;
      first = false;
      /* A union is an operand of a concatenation in parentheses. */
      if(kind() == CONCAT && k.kind() == UNION) {
        out += "(";
        k.render(out);
        out += ")";
      } else
        k.render(out);
    }
    return;
  }
  case STAR: {
    const regex_t &k = node->kids[0];
    /* No parentheses around a single code point. */
    bool bare = false;
    if(k.kind() == ATOM && !k.node->label.empty()) {
      bare = true;
      const std::string &t = k.node->label;
      for(std::size_t i = 1; i < t.size(); ++i)
        if(((unsigned char) t[i] & 0xC0) != 0x80)
          bare = false;
    }
    if(bare)
      out += k.node->label;
    else {
      out += "(";
      k.render(out);
      out += ")";
    }
    out += "*";
    return;
  }
  }
}

/**
 * @brief Operations on regular expressions, shared by @c Regex and
 *        @c TropicalPaths.
 */
struct regex_ops {
  /** @brief A node of kind @p k. */
  static regex_t make(regex_t::Kind k, std::string label,
                      std::vector<regex_t> kids) {
    std::size_t h = std::hash<std::string>()(label) * 31 + k + 1;
    for(const auto &x : kids)
      h = h * 1000003 ^ x.hash();
    regex_t r;
    r.node = std::make_shared<const regex_node>(
      regex_node{k, std::move(label), std::move(kids), h});
    return r;
  }

  /** @brief A one-letter language; @f$\varepsilon@f$ for an empty label. */
  static regex_t atom(const std::string &label) {
    return make(label.empty() ? regex_t::EPSILON : regex_t::ATOM, label, {});
  }

  /** @brief The union of @p v. */
  static regex_t plus(const std::vector<regex_t> &v) {
    std::vector<regex_t> alts;
    bool epsilon = false, starred = false;
    const auto add = [&](const regex_t &a) {
                       switch(a.kind()) {
                       case regex_t::EMPTY:
                         return;
                       case regex_t::EPSILON:
                         epsilon = true;
                         return;
                       case regex_t::STAR:
                         starred = true;
                         break;
                       default:
                         break;
                       }
                       for(const auto &b : alts)
                         if(a == b)
                           return;
                       alts.push_back(a);
                     };
    for(const auto &x : v)
      if(x.kind() == regex_t::UNION)
        for(const auto &a : x.node->kids)
          add(a);
      else
        add(x);
    if(alts.empty())
      return epsilon ? atom("") : regex_t();
    /* ε is in every starred expression already. */
    if(epsilon && !starred)
      alts.insert(alts.begin(), atom(""));
    if(alts.size() == 1)
      return alts[0];
    return make(regex_t::UNION, "", std::move(alts));
  }

  /** @brief The concatenation of @p v, in this order. */
  static regex_t times(const std::vector<regex_t> &v) {
    std::vector<regex_t> factors;
    for(const auto &x : v)
      switch(x.kind()) {
      case regex_t::EMPTY:
        return regex_t();
      case regex_t::EPSILON:
        break;
      case regex_t::CONCAT:
        factors.insert(factors.end(), x.node->kids.begin(),
                       x.node->kids.end());
        break;
      default:
        factors.push_back(x);
      }
    if(factors.empty())
      return atom("");
    if(factors.size() == 1)
      return factors[0];
    return make(regex_t::CONCAT, "", std::move(factors));
  }

  /** @brief The Kleene star of @p a. */
  static regex_t star(const regex_t &a) {
    switch(a.kind()) {
    case regex_t::EMPTY:
    case regex_t::EPSILON:
      return atom("");
    case regex_t::STAR:
      return a;
    case regex_t::UNION: {
      /* (ε | a)* = a* */
      std::vector<regex_t> alts;
      for(const auto &x : a.node->kids)
        if(x.kind() != regex_t::EPSILON)
          alts.push_back(x);
      if(alts.size() == 1)
        return star(alts[0]);
      if(alts.size() < a.node->kids.size())
        return make(regex_t::STAR, "",
                    {make(regex_t::UNION, "", std::move(alts))});
      break;
    }
    default:
      break;
    }
    return make(regex_t::STAR, "", {a});
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
  throw CircuitRefusal(
          PROVSQL_GAP, "semiring-no-monus",
          "The regular-expression semiring computes no monus (outer "
          "joins, EXCEPT and NOT EXISTS need one): its monus, the "
          "difference of languages, needs automata to compute, and would "
          "not register an absence (1 minus a language without the empty "
          "word is 1).");
}
virtual value_type delta(value_type x) const override {
  return x.kind() == regex_t::EMPTY ? zero() : one();
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
