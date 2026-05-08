/**
 * @file semiring/How.h
 * @brief How-provenance m-semiring (canonical polynomial provenance ℕ[X]).
 *
 * The **how-provenance** semiring is the universal commutative semiring
 * for provenance [Green, Karvounarakis, Tannen, *Provenance Semirings*,
 * PODS'07]: every commutative-semiring provenance value can be obtained
 * from the how-provenance polynomial by the unique homomorphism that
 * substitutes each input label with its semiring image.
 *
 * The carrier is the multivariate polynomial semiring
 * @f$\mathbb{N}[X]@f$ over the set @f$X@f$ of base-tuple labels: each
 * value is a *multiset of multisets* of labels, equivalently a sum of
 * monomials with non-negative integer coefficients. We store it in
 * canonical sum-of-products form as
 * @c std::map<std::map<std::string,unsigned>,unsigned>: the outer map
 * sends a monomial (variable→exponent) to its coefficient, and ordered
 * containers give canonical equality on semantically-equal polynomials,
 * which is the value-add over @c sr_formula.
 *
 * Operations:
 * - @c zero()   → @f$0@f$ (empty polynomial)
 * - @c one()    → @f$1@f$ (the constant 1, monomial of degree 0)
 * - @c plus()   → polynomial addition (coefficient-wise sum)
 * - @c times()  → polynomial multiplication (Cauchy product)
 * - @c monus()  → coefficient-wise truncated subtraction
 *                 (per the Lean @c coeff_sub theorem)
 * - @c delta()  → @f$0@f$ if @f$x = 0@f$, else @f$1@f$ (support
 *                 indicator, mirroring @c Counting::delta)
 *
 * This semiring is **not** idempotent and **not** absorptive
 * (@c absorptive() returns @c false), so the evaluator does not
 * deduplicate plus operands. It also does not satisfy
 * left-distributivity of multiplication over monus, contradicting an
 * earlier claim in the literature; see Lean @c How.not_idempotent,
 * @c How.not_absorptive, and @c How.not_mul_sub_left_distributive.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/How.html
 *      Lean 4 verified instance: the @c MvPolynomial X ℕ
 *      @c SemiringWithMonus instance, with proofs of @c How.universal,
 *      @c How.not_idempotent, @c How.not_absorptive, and
 *      @c How.not_mul_sub_left_distributive.
 */
#ifndef HOW_H
#define HOW_H

#include <map>
#include <string>
#include <vector>

#include "Semiring.h"

namespace semiring {

/** @brief A single label identifying a base tuple. */
using how_label_t = std::string;
/** @brief A monomial: each variable mapped to its (positive) exponent. */
using how_monomial_t = std::map<how_label_t, unsigned>;
/** @brief How-provenance value: each monomial mapped to its (positive) coefficient. */
using how_provenance_t = std::map<how_monomial_t, unsigned>;

/**
 * @brief How-provenance m-semiring over @f$\mathbb{N}[X]@f$.
 *
 * Each gate evaluates to a @c how_provenance_t, a polynomial in
 * canonical sum-of-products form. Two semantically-equal polynomials
 * compare equal under @c std::map's lexicographic equality, so this
 * semiring supports provenance-aware query equivalence.
 */
class How : public Semiring<how_provenance_t> {
public:
value_type zero() const override {
  return {};
}

value_type one() const override {
  return { { how_monomial_t{}, 1u } };
}

value_type plus(const std::vector<value_type> &vec) const override {
  value_type result;
  for (const auto &p : vec) {
    for (const auto &[mono, coeff] : p) {
      result[mono] += coeff;
    }
  }
  return result;
}

value_type times(const std::vector<value_type> &vec) const override {
  if (vec.empty()) return one();

  value_type result = vec[0];
  for (size_t i = 1; i < vec.size(); ++i) {
    value_type next;
    for (const auto &[mono1, c1] : result) {
      for (const auto &[mono2, c2] : vec[i]) {
        how_monomial_t combined = mono1;
        for (const auto &[var, exp] : mono2) {
          combined[var] += exp;
        }
        next[combined] += c1 * c2;
      }
    }
    result = std::move(next);
  }
  return result;
}

value_type monus(value_type x, value_type y) const override {
  for (const auto &[mono, c2] : y) {
    auto it = x.find(mono);
    if (it == x.end()) continue;
    if (it->second <= c2)
      x.erase(it);
    else
      it->second -= c2;
  }
  return x;
}

value_type delta(value_type x) const override {
  return x.empty() ? zero() : one();
}

};

}

#endif // HOW_H
