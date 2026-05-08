/**
 * @file semiring/Counting.h
 * @brief Counting semiring (ℕ, +, ×, 0, 1).
 *
 * The counting semiring (@f$\mathbb{N}@f$, @f$+@f$, @f$\times@f$, 0, 1)
 * counts the number of distinct derivations (proof witnesses) of each
 * query result tuple.
 *
 * Operations:
 * - @c zero()   → 0
 * - @c one()    → 1
 * - @c plus()   → sum of all operands
 * - @c times()  → product of all operands
 * - @c monus()  → truncated subtraction: max(0, x − y)
 * - @c delta()  → 1 if x ≠ 0, else 0
 *
 * This semiring is **not** absorptive.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Nat.html
 *      Lean 4 verified instance: @c instSemiringWithMonusNat, with
 *      proofs of @c Nat.mul_sub_left_distributive, @c Nat.not_idempotent,
 *      and @c Nat.not_absorptive.
 */
#ifndef COUNTING_H
#define COUNTING_H

#include <cstdlib>
#include <numeric>
#include <vector>
#include <stdexcept>

#include "Semiring.h"

namespace semiring {
/**
 * @brief The counting semiring over @c unsigned.
 *
 * Each gate evaluates to the number of distinct derivations of the
 * corresponding sub-formula.
 */
class Counting : public semiring::Semiring<unsigned>
{
public:
virtual value_type zero() const override {
  return 0;
}
virtual value_type one() const override {
  return 1;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  return std::accumulate(v.begin(), v.end(), 0);
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return std::accumulate(v.begin(), v.end(), 1, std::multiplies<value_type>());
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x<=y ? 0 : x-y;
}
virtual value_type delta(value_type x) const override
{
  return x!=0 ? 1 : 0;
}
value_type parse_leaf(const char *v) const {
  return atoi(v);
}

};
}

#endif /* COUNTING_H */
