/**
 * @file semiring/BestPath.h
 * @brief One least-cost derivation: its cost and its word of labels.
 *
 * Pairs @f$(c, w)@f$ of a nonnegative cost @f$c@f$ and a word @f$w@f$
 * over the labels of the input tuples, and @f$\mathbb{0}@f$.  A value is
 * better than another if its cost is lower or, at equal costs, if its word
 * comes first in shortlex order (shorter first, then lexicographic over the
 * labels, compared bytewise):
 * - @f$a \oplus b@f$ is the better of the two, so that the values form a
 *   chain, @f$\oplus@f$ being its maximum;
 * - @f$(c, w) \otimes (c', w') = (c + c', w \cdot w')@f$, the
 *   concatenation in the order of the query, so that this semiring does
 *   not commute;
 * - @f$\mathbb{1} = (0, \varepsilon)@f$, the best value, and
 *   @f$\mathbb{0}@f$ the worst;
 * - @f$a \ominus b@f$ is @f$a@f$ if @f$a@f$ is strictly better than
 *   @f$b@f$, @f$\mathbb{0}@f$ otherwise.
 *
 * Shortlex order is compatible with concatenation on both sides, strictly,
 * and so is the order of costs: @f$\otimes@f$ distributes over
 * @f$\oplus@f$ and over @f$\ominus@f$ on both sides.  With
 * @f$\mathbb{1}@f$ the best value, the semiring is absorptive on both
 * sides (@f$a \otimes b \le a@f$ and @f$b \otimes a \le a@f$): a
 * recursion is solved by Dijkstra's algorithm, every star being
 * @f$\mathbb{1}@f$, and a cycle is never followed.  A negative cost would
 * break this, and is refused.
 *
 * Unlike @c TropicalPaths, which keeps every derivation of the least cost,
 * this semiring keeps one, so that it needs no regular expression.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/BestPath.html
 *      Lean 4 verified m-semiring, for costs of any sign, with proofs of
 *      @c BestPath.not_commutative,
 *      @c BestPath.selective_of, @c BestPath.complemented_of,
 *      @c BestPath.mul_sub_left_distributive_of,
 *      @c BestPath.mul_sub_right_distributive_of and, for nonnegative
 *      costs only, @c BestPath.absorptive_of.
 */
#ifndef BEST_PATH_H
#define BEST_PATH_H

#include <limits>
#include <string>
#include <vector>

#include "Semiring.h"
#include "../Circuit.h"

namespace semiring {

/** @brief A cost and a derivation of that cost, as a word of labels. */
struct best_path_t {
  double cost = std::numeric_limits<double>::infinity(); ///< +∞ for 𝟘
  std::vector<std::string> word;                         ///< Its labels

  /** @brief Whether this is @f$\mathbb{0}@f$. */
  bool is_zero() const {
    return cost == std::numeric_limits<double>::infinity();
  }
  /** @brief Equality of the cost and the word. */
  bool operator==(const best_path_t &o) const {
    return cost == o.cost && word == o.word;
  }
  /** @brief Inequality. */
  bool operator!=(const best_path_t &o) const {
    return !(*this == o);
  }
};

/**
 * @brief The one-best-derivation semiring over @c best_path_t.
 *
 * Its leaves combine two mappings, costs and labels (see
 * @c sr_best_path), so they are built by the caller with @c leaf.
 */
class BestPath : public semiring::Semiring<best_path_t>
{
public:
/** @brief Whether @p a is strictly better than @p b. */
static bool better(const best_path_t &a, const best_path_t &b) {
  if(a.cost != b.cost)
    return a.cost < b.cost;
  if(a.word.size() != b.word.size())
    return a.word.size() < b.word.size();
  return a.word < b.word;
}

/** @brief The value of an input of cost @p cost and label @p label, no
 *  label being the empty word, and an infinite cost @f$\mathbb{0}@f$. */
static best_path_t leaf(double cost, const std::string *label) {
  if(!(cost >= 0.))
    throw CircuitRefusal(
            PROVSQL_DELIBERATE, "semiring-negative-cost",
            "sr_best_path requires nonnegative costs: with a negative one, "
            "the empty derivation is no longer the best, and a cycle may "
            "improve a derivation without end.");
  best_path_t r;
  if(cost == std::numeric_limits<double>::infinity())
    return r;   // 𝟘, whatever its label
  r.cost = cost;
  if(label != nullptr)
    r.word.push_back(*label);
  return r;
}

virtual value_type zero() const override {
  return best_path_t();
}
virtual value_type one() const override {
  return best_path_t{0., {}};
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  value_type r;
  for(const auto &x : v)
    if(better(x, r))
      r = x;
  return r;
}
virtual value_type times(const std::vector<value_type> &v) const override {
  value_type r = one();
  for(const auto &x : v) {
    if(x.is_zero())
      return zero();
    r.cost += x.cost;
    r.word.insert(r.word.end(), x.word.begin(), x.word.end());
  }
  return r;
}
virtual value_type monus(value_type x, value_type y) const override {
  return better(x, y) ? x : zero();
}
virtual value_type delta(value_type x) const override {
  return x.is_zero() ? zero() : one();
}
/** @brief @f$\mathbb{1}@f$ is the best value, on both sides of a
 *  product. */
virtual bool absorptive() const override {
  return true;
}
/** @brief The sum is the better of two values. */
virtual bool selective() const override {
  return true;
}
/** @brief Concatenation does not commute. */
virtual bool commutative() const override {
  return false;
}
/**
 * @brief @f$\otimes@f$ distributes over @f$\ominus@f$, on both sides.
 *
 * Read only by the HAVING evaluation, which refuses a non-commutative
 * semiring for now.
 *
 * Lean: @c Provenance.Semirings.BestPath.mul_sub_left_distributive_of.
 */
virtual bool mul_sub_left_distributive() const override {
  return true;
}
/** @brief Every star is @f$\mathbb{1}@f$, by absorption. */
virtual value_type star(const value_type &) const override {
  return one();
}
};

}

#endif /* BEST_PATH_H */
