/**
 * @file semiring/BoolExpr.h
 * @brief Boolean-expression (lineage formula) semiring.
 *
 * The @c BoolExpr semiring represents provenance as a Boolean circuit
 * rather than a scalar value.  Each semiring value is a @c gate_t
 * identifier inside a shared @c BooleanCircuit.  The semiring operations
 * create new gates in that circuit:
 *
 * - @c zero()   → a fresh OR gate with no children (always @c false)
 * - @c one()    → a fresh AND gate with no children (always @c true)
 * - @c plus()   → an OR gate whose children are the operands
 * - @c times()  → an AND gate whose children are the operands
 * - @c monus()  → an AND gate combining @f$x@f$ with a NOT of @f$y@f$
 * - @c delta()  → identity
 *
 * The result of evaluating a circuit over this semiring is the root gate
 * of a new Boolean circuit that encodes the provenance formula.  This
 * circuit can then be compiled to a d-DNNF for probability computation.
 *
 * The semiring is absorptive: duplicate children of OR/AND gates are
 * deduplicated during gate construction.
 *
 * @see https://provsql.org/lean-docs/Provenance/Semirings/BoolFunc.html
 *      Lean 4 verified instance (@c BoolFunc): the algebraic counterpart
 *      of Boolean circuits, with proofs of @c BoolFunc.absorptive,
 *      @c BoolFunc.idempotent, and @c BoolFunc.mul_sub_left_distributive.
 */
#ifndef BOOLEXPR_H
#define BOOLEXPR_H

#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include "Semiring.h"
#include "../BooleanCircuit.h"

namespace semiring {
/**
 * @brief Provenance-as-Boolean-circuit semiring.
 *
 * The carrier type is @c gate_t (a gate identifier in @c BooleanCircuit).
 * Evaluating the provenance circuit over this semiring constructs a new
 * Boolean circuit expressing the provenance formula.
 */
class BoolExpr : public Semiring<gate_t> {
using value_t = gate_t; ///< Carrier type: a gate ID in the target BooleanCircuit

BooleanCircuit &c;      ///< The Boolean circuit being constructed
const gate_t ZERO; ///< Pre-allocated zero gate (OR with no children)
const gate_t ONE;  ///< Pre-allocated one gate (AND with no children)

public:
/**
 * @brief Construct a BoolExpr semiring over the given circuit.
 * @param bc  The Boolean circuit in which semiring operations create new gates.
 */
BoolExpr(BooleanCircuit &bc) : c(bc), ZERO(c.setGate(BooleanGate::OR)), ONE(c.setGate(BooleanGate::AND)) {
}

value_type zero() const override {
  return ZERO;
}

value_type one() const override {
  return ONE;
}

value_type plus(const std::vector<value_type> &vec) const override {
  if(vec.empty())
    return ZERO;

  auto g = c.setGate(BooleanGate::OR);

  std::set<gate_t> seen;
  for (const auto &h : vec) {
    if(seen.find(h)!=seen.end())
      continue;
    seen.insert(h);
    c.addWire(g, h);
  }
  return g;
}

value_type times(const std::vector<value_type> &vec) const override {
  if(vec.empty())
    return ONE;

  auto g = c.setGate(BooleanGate::AND);

  std::set<gate_t> seen;
  for (const auto &h : vec) {
    if(seen.find(h)!=seen.end())
      continue;
    seen.insert(h);
    c.addWire(g, h);
  }
  return g;
}

virtual value_type monus(value_type x, value_type y) const override {
  auto g2 = c.setGate(BooleanGate::NOT);
  c.addWire(g2,y);

  if(x==ONE)
    return g2;
  else {
    auto g1 = c.setGate(BooleanGate::AND);
    c.addWire(g1,x);
    c.addWire(g1,g2);
    return g1;
  }
}

value_type delta(value_type x) const override {
  return x;
}

virtual bool absorptive() const override {
  return true;
}
/**
 * @brief @c BoolExpr is the free Boolean-circuit construction; the
 *        evaluation map to @c Bool at any valuation is an m-semiring
 *        homomorphism, so the safe-query Boolean rewrite preserves
 *        semantics.  Lean: @c BoolFunc is the algebraic counterpart;
 *        the relevant theorems live in
 *        @c Provenance.Semirings.BoolFunc and
 *        @c Provenance.Semirings.Bool.homomorphism_to_BoolFunc.
 */
virtual bool compatibleWithBooleanRewrite() const override {
  return true;
}

/**
 * @brief @c BoolExpr persists the d-DNNF certificate on the HAVING
 *        possible-worlds enumerations it builds (deterministic ORs over
 *        decomposable world terms), so the certificate-aware evaluators
 *        (@c independentEvaluation, @c interpretAsDD) handle the
 *        resulting circuits linearly.
 */
virtual bool certifying() const override {
  return true;
}

/**
 * @brief A gate qualifies as an independent literal when it is a base
 *        Bernoulli input, or a constant (the empty AND / OR): distinct
 *        such gates have disjoint supports, so ANDs over them are
 *        decomposable.  Derived contributors (internal sub-circuits,
 *        @c mulinput literals whose block siblings are correlated) do
 *        not qualify.
 */
virtual bool independent_literal(const value_type &g) const override {
  const auto t = c.getGateType(g);
  if (t == BooleanGate::IN)
    return true;
  return (t == BooleanGate::AND || t == BooleanGate::OR) &&
         c.getWires(g).empty();
}

/**
 * @brief One complete world: AND of the @p present literals and of
 *        NOT-gates over the @p missing ones, marked decomposable
 *        (pairwise-distinct independent literals have disjoint
 *        supports).  The negations are De Morgan-expanded per literal
 *        -- rather than the @c monus(one, plus(missing)) form the
 *        uncertified construction uses -- so the whole term lies
 *        inside one certified island and the sharing of contributors
 *        across the (mutually exclusive) world terms is licensed by
 *        the certificate.  NOT-gates are cached per literal: the same
 *        contributor is negated in up to half the worlds.
 */
virtual value_type certified_world_term(
  const std::vector<value_type> &present,
  const std::vector<value_type> &missing) const override {
  if (missing.empty() && present.size() == 1)
    return present[0];
  auto g = c.setGate(BooleanGate::AND);
  c.setInfo(g, DNNF_CERT_INFO);
  for (const auto &h : present)
    c.addWire(g, h);
  for (const auto &h : missing) {
    auto it = not_cache.find(h);
    if (it == not_cache.end()) {
      auto n = c.setGate(BooleanGate::NOT);
      c.addWire(n, h);
      it = not_cache.emplace(h, n).first;
    }
    c.addWire(g, it->second);
  }
  return g;
}

/**
 * @brief Deterministic OR over pairwise-exclusive world terms, marked
 *        with the d-DNNF certificate.  No child deduplication: distinct
 *        complete worlds yield structurally distinct terms.
 */
virtual value_type certified_exclusive_plus(
  const std::vector<value_type> &vec) const override {
  if (vec.size() == 1)
    return vec[0];
  auto g = c.setGate(BooleanGate::OR);
  c.setInfo(g, DNNF_CERT_INFO);
  for (const auto &h : vec)
    c.addWire(g, h);
  return g;
}

private:
/** @brief Per-literal NOT gates of @c certified_world_term (shared across world terms). */
mutable std::map<gate_t, gate_t> not_cache;
};
}

#endif // BOOLEXPR_H
