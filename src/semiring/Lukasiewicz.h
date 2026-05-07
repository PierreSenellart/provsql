/**
 * @file semiring/Lukasiewicz.h
 * @brief Łukasiewicz fuzzy m-semiring over @f$[0,1]@f$.
 *
 * The Łukasiewicz m-semiring (@f$[0,1]@f$, @f$\max@f$,
 * @f$\otimes_{\text{Ł}}@f$, 0, 1) is used to model graded-truth
 * provenance: input gates carry degree-of-evidence values in
 * @f$[0,1]@f$, @f$\oplus = \max@f$ keeps the strongest alternative,
 * and @f$\otimes_{\text{Ł}}(a,b) = \max(a + b - 1, 0)@f$ is the
 * Łukasiewicz t-norm, the bounded-loss conjunction.
 *
 * Operations:
 * - @c zero()   → 0
 * - @c one()    → 1
 * - @c plus()   → maximum of all operands (empty list → 0)
 * - @c times()  → @f$\max(\sum_i a_i - (n-1), 0)@f$ for @f$n@f$
 *                 operands (empty list → 1)
 * - @c monus()  → 0 if @f$a \le b@f$, @f$a@f$ otherwise
 * - @c delta()  → 1 if @c x is non-zero, else 0
 *
 * Absorptivity: `absorptive()` returns `true`. With inputs in
 * @f$[0,1]@f$, @f$\mathbb{1} \oplus a = \max(1, a) = 1@f$.
 * The circuit evaluator may exploit the resulting idempotency to
 * deduplicate operands.
 *
 * Compared to Viterbi (which uses @f$a \cdot b@f$ for ⊗): the
 * Łukasiewicz t-norm preserves crisp truth (@f$0.7 \otimes 1 = 0.7@f$)
 * and does not collapse long conjunctions to near-zero, making it
 * the standard fuzzy choice for graded but non-probabilistic
 * conjunctions.
 *
 * @note No bounds checking is performed: it is the caller's
 * responsibility to supply input values in @f$[0,1]@f$.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Lukasiewicz.html
 *      Lean 4 verified instance: @c instSemiringWithMonusLukasiewicz,
 *      with proofs of @c Lukasiewicz.absorptive,
 *      @c Lukasiewicz.idempotent, and
 *      @c Lukasiewicz.mul_sub_left_distributive.
 */
#ifndef LUKASIEWICZ_H
#define LUKASIEWICZ_H

#include <algorithm>
#include <numeric>
#include <vector>

#include "Semiring.h"

namespace semiring {
/**
 * @brief The Łukasiewicz fuzzy m-semiring over @c double.
 *
 * Each gate evaluates to a degree of evidence in @f$[0,1]@f$ under the
 * Łukasiewicz t-norm.
 */
class Lukasiewicz : public semiring::Semiring<double>
{
public:
virtual value_type zero() const override {
  return 0.0;
}
virtual value_type one() const override {
  return 1.0;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  if(v.empty()) return zero();
  return *std::max_element(v.begin(), v.end());
}
virtual value_type times(const std::vector<value_type> &v) const override {
  if(v.empty()) return one();
  double s = std::accumulate(v.begin(), v.end(), 0.0);
  double r = s - static_cast<double>(v.size() - 1);
  return r > 0.0 ? r : 0.0;
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x<=y ? zero() : x;
}
virtual value_type delta(value_type x) const override
{
  return x!=zero() ? one() : zero();
}
virtual bool absorptive() const override {
  return true;
}

};
}

#endif /* LUKASIEWICZ_H */
