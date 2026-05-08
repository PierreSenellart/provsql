/**
 * @file semiring/Why.h
 * @brief Why-provenance semiring (set of witness sets).
 *
 * The **why-provenance** semiring represents provenance as a set of
 * *witness sets*.  Each witness set is a set of base-tuple labels that
 * together "witness" one derivation of the query result.  The full
 * provenance is the collection of all such witness sets.
 *
 * Formally, the carrier type is @f$\mathcal{P}(\mathcal{P}(\text{Labels}))@f$,
 * implemented as @c why_provenance_t = @c std::set<std::set<std::string>>.
 *
 * Operations:
 * - @c zero()   → ∅ (no derivations)
 * - @c one()    → {∅} (one derivation requiring no witnesses)
 * - @c plus()   → union of input sets
 * - @c times()  → pairwise concatenation (Cartesian product of witnesses)
 * - @c monus()  → remove elements of @f$y@f$ from @f$x@f$
 * - @c delta()  → identity (returns @f$x@f$ unchanged if non-empty)
 *
 * This semiring is idempotent (set union is idempotent: @f$a \oplus a = a@f$),
 * but **not** absorptive in the @f$\mathbb{1} \oplus a = \mathbb{1}@f$ sense
 * used by @c absorptive(): @f${\{\emptyset\}} \cup \{\{x\}\} = \{\emptyset, \{x\}\} \neq \{\emptyset\}@f$.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Why.html
 *      Lean 4 verified instance: @c instSemiringWithMonusWhy, with
 *      proofs of @c Why.idempotent, @c Why.not_absorptive, and
 *      @c Why.not_mul_sub_left_distributive.
 */
#ifndef WHY_H
#define WHY_H

#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include "Semiring.h"

namespace semiring {

/** @brief A single label identifying a base tuple. */
using label_t = std::string;
/** @brief A witness: a set of labels that collectively justify one derivation. */
using label_set = std::set<label_t>;
/** @brief Why-provenance value: the full set of all witnesses. */
using why_provenance_t = std::set<label_set>;

/**
 * @brief Why-provenance semiring.
 *
 * Each gate evaluates to a @c why_provenance_t (set of witness sets).
 */
class Why : public Semiring<why_provenance_t> {
public:
// Additive identity
value_type zero() const override {
  return {};
}

// Multiplicative identity: empty set (⊗(x,{{}}) means "don't change")
value_type one() const override {
  return { {} };
}

// Union of all input sets
value_type plus(const std::vector<value_type> &vec) const override {
  value_type result;
  for (const auto &v : vec) {
    result.insert(v.begin(), v.end());
  }
  return result;
}

// Cartesian product: union each inner set with each other
value_type times(const std::vector<value_type> &vec) const override {
  if (vec.empty()) return one();

  value_type result = vec[0];
  for (size_t i = 1; i < vec.size(); ++i) {
    value_type temp;
    for (const auto &s1 : result) {
      for (const auto &s2 : vec[i]) {
        label_set combined = s1;
        combined.insert(s2.begin(), s2.end());
        temp.insert(std::move(combined));
      }
    }
    result = std::move(temp);
  }
  return result;
}


virtual value_type monus(value_type x, value_type y) const override {
  for (auto const &s : y) {
    x.erase(s);
  }
  return x;
}

value_type delta(value_type x) const override {
  return x.empty() ? zero() : x;
}

value_type parse_leaf(const char *v) const {
  if(strchr(v, '{'))
    throw SemiringException("Complex Why-semiring values for input tuples not currently supported.");
  label_set single;
  single.insert(std::string(v));
  value_type result;
  result.insert(std::move(single));
  return result;
}

std::string to_text(const value_type &prov) const {
  std::ostringstream oss;
  oss << "{";
  bool firstOuter = true;
  for (const auto &inner : prov) {
    if (!firstOuter) oss << ",";
    firstOuter = false;
    oss << "{";
    bool firstInner = true;
    for (const auto &label : inner) {
      if (!firstInner) oss << ",";
      firstInner = false;
      oss << label;
    }
    oss << "}";
  }
  oss << "}";
  return oss.str();
}

};

}

#endif // WHY_H
