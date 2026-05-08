/**
 * @file semiring/Tropical.h
 * @brief Tropical (min-plus) m-semiring over @f$\mathbb{R} \cup \{+\infty\}@f$.
 *
 * The tropical m-semiring (@f$\mathbb{R} \cup \{+\infty\}@f$,
 * @f$\min@f$, @f$+@f$, @f$+\infty@f$, 0) is used to model
 * shortest-path/least-cost provenance: input gates carry edge weights
 * (or costs), @f$\oplus = \min@f$ selects the cheapest derivation,
 * and @f$\otimes = +@f$ accumulates cost along a derivation.
 *
 * Operations:
 * - @c zero()   → @f$+\infty@f$
 * - @c one()    → 0
 * - @c plus()   → minimum of all operands (empty list → @f$+\infty@f$)
 * - @c times()  → sum of all operands (empty list → 0)
 * - @c monus()  → @f$+\infty@f$ if @f$x \ge y@f$ in the usual order,
 *                 @f$x@f$ otherwise (note this is the *reverse* of the
 *                 natural semiring order; see Lean reference)
 * - @c delta()  → @f$+\infty@f$ if @c x is @f$+\infty@f$, else 0
 *
 * Absorptivity: `absorptive()` returns `false`. The Lean formalisation
 * proves absorptivity only for canonically-ordered carriers (e.g.
 * @f$\mathbb{N}@f$); over arbitrary @c double values (including
 * negatives) @f$\mathbb{1} \oplus a = \min(0, a)@f$ is not always 0.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Tropical.html
 *      Lean 4 verified instance: @c instSemiringWithMonusTropicalWithTop,
 *      with proofs of @c Tropical.absorptive (under
 *      @c CanonicallyOrderedAdd) and
 *      @c Tropical.mul_sub_left_distributive.
 */
#ifndef TROPICAL_H
#define TROPICAL_H

#include <algorithm>
#include <limits>
#include <numeric>
#include <vector>

#include "Semiring.h"

namespace semiring {
/**
 * @brief Tropical (min-plus) m-semiring over @c double.
 *
 * Each gate evaluates to a real-valued cost (with @f$+\infty@f$ as
 * the additive identity). Inputs are read from the mapping table as
 * %float8 values; pass <tt>'Infinity'::%float8</tt> to encode the
 * additive zero.
 */
class Tropical : public semiring::Semiring<double>
{
public:
virtual value_type zero() const override {
  return std::numeric_limits<double>::infinity();
}
virtual value_type one() const override {
  return 0.0;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  if(v.empty()) return zero();
  return *std::min_element(v.begin(), v.end());
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return std::accumulate(v.begin(), v.end(), 0.0);
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x>=y ? zero() : x;
}
virtual value_type delta(value_type x) const override
{
  return x==zero() ? zero() : one();
}
value_type parse_leaf(const char *v) const {
  return atof(v);
}

};
}

#endif /* TROPICAL_H */
