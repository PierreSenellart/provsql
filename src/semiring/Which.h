/**
 * @file semiring/Which.h
 * @brief Which-provenance (lineage) m-semiring.
 *
 * The **which-provenance** semiring (also known as *lineage*) records,
 * for each derivation of a result tuple, the set of base-tuple labels
 * that contributed to it.  Unlike why-provenance, only one such set is
 * tracked per gate: the union of all witnesses across all derivations.
 *
 * Formally, the carrier set is
 * @f$\mathcal{P}(\text{Labels}) \uplus \{\bot\}@f$, where @f$\bot@f$
 * (the additive identity) represents "no derivation".  Implemented as
 * @c which_provenance_t = @c std::optional<std::set<std::string>>
 * (an empty optional is @f$\bot@f$).
 *
 * Operations:
 * - @c zero()   → @f$\bot@f$ (no derivations)
 * - @c one()    → @f$\emptyset@f$ (a derivation requiring no witnesses)
 * - @c plus()   → union of witness sets; @f$\bot@f$ acts as identity
 * - @c times()  → union of witness sets; @f$\bot@f$ is absorbing
 * - @c monus()  → set difference, @f$\bot@f$ when @f$x \subseteq y@f$
 * - @c delta()  → identity (@f$\bot \mapsto \bot@f$, @f$x \mapsto x@f$)
 *
 * The semiring is idempotent (set union is idempotent: @f$a \oplus a = a@f$)
 * but **not** absorptive in the @f$\mathbb{1} \oplus a = \mathbb{1}@f$ sense
 * used by @c absorptive(): @f$\emptyset \oplus \{x\} = \{x\} \neq \emptyset@f$.
 * Times also does not left-distribute over monus as long as the label
 * universe is non-empty.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/Which.html
 *      Lean 4 verified instance: @c instSemiringWithMonusWhich, with
 *      proofs of @c Which.idempotent, @c Which.not_absorptive, and
 *      @c Which.not_mul_sub_left_distributive.
 */
#ifndef WHICH_H
#define WHICH_H

#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Semiring.h"

namespace semiring {

/** @brief Which-provenance value: a set of labels, or @f$\bot@f$ (empty optional). */
using which_provenance_t = std::optional<std::set<std::string> >;

/**
 * @brief Which-provenance (lineage) semiring.
 *
 * Each gate evaluates to a @c which_provenance_t (set of contributing
 * labels, or @f$\bot@f$).
 */
class Which : public Semiring<which_provenance_t> {
public:
value_type zero() const override {
  return std::nullopt;
}

value_type one() const override {
  return std::set<std::string>{};
}

value_type plus(const std::vector<value_type> &vec) const override {
  value_type result = std::nullopt;
  for (const auto &v : vec) {
    if(!v.has_value())
      continue;
    if(!result.has_value())
      result = std::set<std::string>{};
    result->insert(v->begin(), v->end());
  }
  return result;
}

value_type times(const std::vector<value_type> &vec) const override {
  if(vec.empty())
    return one();

  value_type result = std::set<std::string>{};
  for (const auto &v : vec) {
    if(!v.has_value())
      return std::nullopt;
    result->insert(v->begin(), v->end());
  }
  return result;
}

value_type monus(value_type x, value_type y) const override {
  if(!x.has_value())
    return std::nullopt;
  if(!y.has_value())
    return x;
  if(std::includes(y->begin(), y->end(), x->begin(), x->end()))
    return std::nullopt;
  std::set<std::string> diff;
  std::set_difference(
    x->begin(), x->end(),
    y->begin(), y->end(),
    std::inserter(diff, diff.end())
    );
  return diff;
}

value_type delta(value_type x) const override {
  return x;
}

value_type parse_leaf(const char *v) const {
  if(strchr(v, '{'))
    throw SemiringException("Complex Which-semiring values for input tuples not currently supported.");
  std::set<std::string> single;
  single.insert(std::string(v));
  return value_type(std::move(single));
}

std::string to_text(const value_type &prov) const {
  std::ostringstream oss;
  if(!prov.has_value()) {
    oss << "⊥";
  } else {
    oss << "{";
    bool first = true;
    for (const auto &label : *prov) {
      if (!first) oss << ",";
      first = false;
      oss << label;
    }
    oss << "}";
  }
  return oss.str();
}
};

}

#endif // WHICH_H
