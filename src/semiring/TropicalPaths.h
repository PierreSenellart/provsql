/**
 * @file semiring/TropicalPaths.h
 * @brief Min-cost derivations with the derivations of that cost.
 *
 * Pairs @f$(c, L)@f$ of a cost @f$c \in \mathbb{R} \cup \{+\infty\}@f$
 * and a language @f$L@f$ over the labels of the input tuples, the
 * derivations of cost @f$c@f$, written as a regular expression (@c
 * Regex.h):
 * - @f$(c, L) \oplus (c', L')@f$ keeps the lower cost, with its
 *   derivations, and both languages' union on a tie;
 * - @f$(c, L) \otimes (c', L') = (c + c', L \cdot L')@f$, the
 *   concatenation in the order of the query, so that this semiring does
 *   not commute;
 * - @f$\mathbb{0} = (+\infty, \emptyset)@f$, @f$\mathbb{1} = (0,
 *   \{\varepsilon\})@f$.
 *
 * On a path query, the value of a tuple is the cost of its shortest paths
 * and those paths.  A cycle of positive cost never lies on a shortest path
 * (@f$(c, L)^* = \mathbb{1}@f$); one of cost zero can be followed any
 * number of times at no cost (@f$(0, L)^* = (0, L^*)@f$), which a regular
 * expression still writes finitely; one of negative cost makes the cost
 * unbounded, and is refused.
 */
#ifndef TROPICAL_PATHS_H
#define TROPICAL_PATHS_H

#include <limits>
#include <string>
#include <vector>

#include "Regex.h"

namespace semiring {

/** @brief A cost, and the derivations of that cost. */
struct tropical_paths_t {
  double cost = std::numeric_limits<double>::infinity(); ///< Least cost
  regex_t paths;                                         ///< Its derivations

  /** @brief Syntactic equality, as for @c regex_t. */
  bool operator==(const tropical_paths_t &o) const {
    return cost == o.cost && paths == o.paths;
  }
  /** @brief Syntactic inequality. */
  bool operator!=(const tropical_paths_t &o) const {
    return !(*this == o);
  }
};

/**
 * @brief The min-cost-paths semiring over @c tropical_paths_t.
 *
 * Its leaves combine two mappings, costs and labels (see
 * @c sr_tropical_paths), so they are built by the caller rather than
 * parsed from one mapping value.
 */
class TropicalPaths : public semiring::Semiring<tropical_paths_t>
{
public:
virtual value_type zero() const override {
  return tropical_paths_t();
}
virtual value_type one() const override {
  return tropical_paths_t{0., regex_ops::atom("")};
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  value_type r;
  for(const auto &x : v)
    if(x.cost < r.cost)
      r.cost = x.cost;
  if(r.cost == zero().cost)
    return zero();
  std::vector<regex_t> best;
  for(const auto &x : v)
    if(x.cost == r.cost)
      best.push_back(x.paths);
  r.paths = regex_ops::plus(best);
  return r;
}
virtual value_type times(const std::vector<value_type> &v) const override {
  value_type r = one();
  std::vector<regex_t> words;
  for(const auto &x : v) {
    if(x.cost == zero().cost || x.paths.kind() == regex_t::EMPTY)
      return zero();
    r.cost += x.cost;
    words.push_back(x.paths);
  }
  r.paths = regex_ops::times(words);
  return r;
}
virtual value_type monus(value_type, value_type) const override {
  throw SemiringException(
          "The min-cost-paths semiring has no monus.");
}
virtual value_type delta(value_type x) const override {
  return x.cost == zero().cost ? zero() : one();
}
/** @brief A tie keeps the union of the derivations, itself idempotent. */
virtual bool idempotent() const override {
  return true;
}
/** @brief Concatenation does not commute. */
virtual bool commutative() const override {
  return false;
}
/** @brief Every value of nonnegative cost has a star. */
virtual bool has_star() const override {
  return true;
}
virtual value_type star(const value_type &a) const override {
  if(a.cost > 0)
    return one();
  if(a.cost == 0)
    return tropical_paths_t{0., regex_ops::star(a.paths)};
  throw SemiringException(
          "A cycle of negative cost makes the least cost unbounded, with no "
          "shortest derivation.");
}
/** @brief Equality of the derivations is syntactic. */
virtual bool exact_equality() const override {
  return false;
}
};

}

#endif /* TROPICAL_PATHS_H */
