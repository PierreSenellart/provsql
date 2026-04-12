/**
 * @file semiring/Boolean.h
 * @brief Boolean semiring ({false, true}, ∨, ∧, false, true).
 *
 * The Boolean semiring is the simplest semiring supported by ProvSQL.
 * Provenance evaluates to @c true if at least one derivation of the
 * tuple exists in the current database instance (i.e., the query answer
 * is "certain"), and @c false otherwise.
 *
 * The semiring is absorptive (∨ is idempotent), so the circuit evaluator
 * can safely deduplicate children of OR gates.
 *
 * Operations:
 * - @c zero()   → @c false
 * - @c one()    → @c true
 * - @c plus()   → logical OR (any_of)
 * - @c times()  → logical AND (all_of)
 * - @c monus()  → @f$x \;\&\; \lnot y@f$
 * - @c delta()  → identity
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Bool.html
 *      Lean 4 verified instance: @c instSemiringWithMonusBool, with
 *      proofs of @c Bool.absorptive, @c Bool.idempotent, and
 *      @c Bool.mul_sub_left_distributive.
 */
#ifndef BOOLEAN_H
#define BOOLEAN_H

#include <algorithm>
#include <vector>

#include "Semiring.h"

namespace semiring {
/**
 * @brief The Boolean semiring over @c bool.
 *
 * Provides the standard Boolean interpretation of provenance circuits.
 */
class Boolean : public semiring::Semiring<bool>
{
public:
virtual value_type zero() const override {
  return false;
}
virtual value_type one() const override {
  return true;
}
virtual value_type plus(const std::vector<value_type> &v) const override {
  return std::any_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type times(const std::vector<value_type> &v) const override {
  return std::all_of(v.begin(), v.end(), [](bool x) {
        return x;
      });
}
virtual value_type monus(value_type x, value_type y) const override
{
  return x & !y;
}
virtual value_type delta(value_type x) const override
{
  return x;
}
virtual bool absorptive() const override {
  return true;
}
};
}

#endif /* BOOLEAN_H */
