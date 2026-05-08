/**
 * @file semiring/Viterbi.h
 * @brief Viterbi (max-times) m-semiring over @f$[0,1]@f$.
 *
 * The Viterbi m-semiring (@f$[0,1]@f$, @f$\max@f$, @f$\times@f$, 0, 1)
 * is used to model most-likely-derivation provenance: input gates
 * carry probabilities, @f$\oplus = \max@f$ keeps the most likely
 * derivation, and @f$\otimes = \times@f$ multiplies probabilities
 * along a derivation.
 *
 * Operations:
 * - @c zero()   → 0
 * - @c one()    → 1
 * - @c plus()   → maximum of all operands (empty list → 0)
 * - @c times()  → product of all operands (empty list → 1)
 * - @c monus()  → 0 if @f$a \le b@f$, @f$a@f$ otherwise
 * - @c delta()  → 1 if @c x is non-zero, else 0
 *
 * Absorptivity: `absorptive()` returns `true`. With inputs in
 * @f$[0,1]@f$, @f$\mathbb{1} \oplus a = \max(1, a) = 1@f$.
 * The circuit evaluator may exploit the resulting idempotency to
 * deduplicate operands.
 *
 * @note No bounds checking is performed: it is the caller's
 * responsibility to supply input probabilities in @f$[0,1]@f$.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Viterbi.html
 *      Lean 4 verified instance: @c instSemiringWithMonusViterbi,
 *      with proofs of @c Viterbi.absorptive,
 *      @c Viterbi.idempotent, and
 *      @c Viterbi.mul_sub_left_distributive.
 */
#ifndef VITERBI_H
#define VITERBI_H

#include <algorithm>
#include <numeric>
#include <vector>

#include "Semiring.h"

namespace semiring {
/**
 * @brief The Viterbi (max-times) m-semiring over @c double.
 *
 * Each gate evaluates to the probability of the most likely derivation
 * of the corresponding sub-formula.
 */
class Viterbi : public semiring::Semiring<double>
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
  return std::accumulate(v.begin(), v.end(), 1.0, std::multiplies<value_type>());
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
value_type parse_leaf(const char *v) const {
  return atof(v);
}

};
}

#endif /* VITERBI_H */
