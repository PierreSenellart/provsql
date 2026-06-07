/**
 * @file semiring/Semiring.h
 * @brief Abstract semiring interface for provenance evaluation.
 *
 * ProvSQL evaluates provenance circuits over arbitrary (m-)semirings.
 * This header defines the abstract base class @c semiring::Semiring<V>
 * that every concrete semiring must implement.
 *
 * A **semiring** @f$(S, \oplus, \otimes, \mathbb{0}, \mathbb{1})@f$
 * consists of:
 * - A carrier set @f$S@f$ (the @c value_type).
 * - An additive operation @f$\oplus@f$ with identity @f$\mathbb{0}@f$.
 * - A multiplicative operation @f$\otimes@f$ with identity @f$\mathbb{1}@f$.
 *
 * An **m-semiring** additionally provides:
 * - A monus operation @f$\ominus@f$ used for set-difference queries.
 * - A @f$\delta@f$ operator.
 *
 * Optional operations (comparison, semimodule scalar multiplication,
 * aggregation, and value literals) are provided by subclasses that
 * support them; the base class throws @c SemiringException for all of
 * these.
 *
 * Concrete implementations live in the same @c semiring/ directory:
 * @c Boolean.h, @c Counting.h, @c Formula.h, @c Why.h, @c BoolExpr.h.
 *
 * @see https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html
 *      Lean 4 formalization of the @c SemiringWithMonus typeclass and
 *      proofs of the key monus identities (@c monus_smallest,
 *      @c monus_self, @c zero_monus, @c monus_add, @c add_monus,
 *      @c idempotent_iff_add_monus).
 */
#ifndef SEMIRING_H
#define SEMIRING_H

#include <vector>
#include <string>

#include "../Aggregation.h"

namespace semiring {

/**
 * @brief Exception thrown when a semiring operation is not supported.
 *
 * Raised by the default implementations of optional operations
 * (@c cmp, @c semimod, @c agg, @c value) when a subclass does not
 * override them.
 */
class SemiringException : public std::exception
{
std::string message; ///< Human-readable description of the error

public:
/**
 * @brief Construct with a descriptive error message.
 * @param m  Error message.
 */
SemiringException(const std::string &m) : message(m) {
}
/**
 * @brief Return the error message as a C-string.
 * @return Null-terminated error description.
 */
virtual char const * what() const noexcept {
  return message.c_str();
}
};

/**
 * @brief Abstract base class for (m-)semirings.
 *
 * @tparam V  The carrier type (e.g. @c bool, @c unsigned, @c std::string).
 *
 * ### Required operations
 * All pure-virtual methods must be implemented by concrete subclasses.
 *
 * ### Optional operations
 * @c cmp(), @c semimod(), @c agg(), and @c value() have default
 * implementations that throw @c SemiringException.  Override them in
 * subclasses that support these circuit gate types.
 *
 * ### Absorptive semirings
 * A semiring is *absorptive* (i.e.,
 * @f$\mathbb{1} \oplus a = \mathbb{1}@f$ for all @f$a@f$) iff
 * @c absorptive() returns @c true.  Absorptivity implies idempotency
 * (@f$a \oplus a = a@f$), which lets the circuit evaluator and the
 * HAVING-semantics machinery deduplicate operands and short-circuit
 * over the multiplicative identity.
 */
template<typename V>
class Semiring
{
public:
/** @brief The carrier type of this semiring. */
typedef V value_type;

/**
 * @brief Return the additive identity @f$\mathbb{0}@f$.
 * @return The zero element of the semiring.
 */
virtual value_type zero() const = 0;

/**
 * @brief Return the multiplicative identity @f$\mathbb{1}@f$.
 * @return The one element of the semiring.
 */
virtual value_type one() const = 0;

/**
 * @brief Apply the additive operation to a list of values.
 *
 * @param v  Ordered list of operands (empty list should return @c zero()).
 * @return   @f$v_0 \oplus v_1 \oplus \cdots@f$.
 */
virtual value_type plus(const std::vector<value_type> &v) const = 0;

/**
 * @brief Apply the multiplicative operation to a list of values.
 *
 * @param v  Ordered list of operands (empty list should return @c one()).
 * @return   @f$v_0 \otimes v_1 \otimes \cdots@f$.
 */
virtual value_type times(const std::vector<value_type> &v) const = 0;

/**
 * @brief Apply the monus (m-semiring difference) operation.
 *
 * @param x  Minuend.
 * @param y  Subtrahend.
 * @return   @f$x \ominus y@f$.
 */
virtual value_type monus(value_type x, value_type y) const = 0;

/**
 * @brief Apply the @f$\delta@f$ operator.
 *
 * @param x  Input value.
 * @return   @f$\delta(x)@f$.
 */
virtual value_type delta(value_type x) const = 0;

/**
 * @brief Evaluate a comparison gate.
 *
 * @param s1  Left operand.
 * @param op  Comparison operator.
 * @param s2  Right operand.
 * @return    Result of the comparison in this semiring.
 * @throws SemiringException if not overridden.
 */
virtual value_type cmp(value_type s1, ComparisonOperator op, value_type s2) const {
  throw SemiringException("This semiring does not support cmp gates.");
}

/**
 * @brief Apply a semimodule scalar multiplication.
 *
 * @param x  Provenance value.
 * @param s  Scalar value.
 * @return   @f$x * s@f$ in the semimodule.
 * @throws SemiringException if not overridden.
 */
virtual value_type semimod(value_type x, value_type s) const {
  throw SemiringException("This semiring does not support semimod gates.");
}

/**
 * @brief Evaluate an aggregation gate.
 *
 * @param op  The aggregation function (COUNT, SUM, MIN…).
 * @param s   List of child semiring values to aggregate.
 * @return    The aggregated value.
 * @throws SemiringException if not overridden.
 */
virtual value_type agg(AggregationOperator op, const std::vector<value_type> &s) {
  throw SemiringException("This semiring does not support agg gates.");
}

/**
 * @brief Interpret a literal string as a semiring value.
 *
 * Used for @c gate_value gates whose payload is a string.
 *
 * @param s  Literal string.
 * @return   The corresponding semiring value.
 * @throws SemiringException if not overridden.
 */
virtual value_type value(const std::string &s) const {
  throw SemiringException("This semiring does not support value gates.");
}

virtual ~Semiring() = default;

/**
 * @brief Return @c true if this semiring is absorptive
 *        (@f$\mathbb{1} \oplus a = \mathbb{1}@f$ for all @f$a@f$).
 *
 * When @c true, the circuit evaluator and HAVING-semantics machinery
 * may exploit the resulting idempotency (@f$a \oplus a = a@f$, implied
 * by absorptivity) to deduplicate children of @c plus gates and to
 * short-circuit over the multiplicative identity.
 *
 * @return @c false by default; override to return @c true.
 */
virtual bool absorptive() const {
  return false;
}

/**
 * @brief Return @c true if a semiring homomorphism @c BoolFunc(X)
 *        →+* @c S exists, so the safe-query (Boolean-rewrite)
 *        optimisation produces circuits that are semantically faithful
 *        when evaluated under this semiring.
 *
 * The compiled-semiring dispatcher consults this predicate before
 * evaluating a circuit whose root gate carries
 * @c PROVSQL_ROOT_TAG_BOOLEAN_REWRITE.  Returning @c false on a tagged
 * circuit raises @c CircuitException.
 *
 * Defaults to @c false: a new semiring whose author has not yet
 * verified the homomorphism is fail-closed by construction.
 * Subclasses with a verified homomorphism (currently @c Boolean,
 * @c BoolExpr, @c Formula, and @c IntervalUnion) override to return
 * @c true.  The justification (Lean-proof reference) belongs in a
 * comment next to each override; see the @c src/semiring/ headers.
 */
virtual bool compatibleWithBooleanRewrite() const {
  return false;
}

/**
 * @brief Whether this semiring builds *certified* exclusive
 *        enumerations (see the three hooks below).
 *
 * The HAVING possible-worlds machinery (@c provsql_having) enumerates,
 * for a comparison over a group aggregate, the complete valuations of
 * the group's contributors that satisfy the predicate.  Those
 * disjuncts partition the worlds -- the OR is *deterministic* and each
 * world term an AND of literals over distinct contributors,
 * *decomposable* -- i.e., the enumeration is a d-DNNF by construction.
 * A circuit-building semiring can persist that knowledge as the d-DNNF
 * certificate (the same mark the bounded-treewidth reachability route
 * emits), letting the linear certificate-aware evaluators handle the
 * result; scalar semirings have nothing to mark.
 *
 * @return @c false by default (the hooks below then go unused and the
 *         historical constructions are kept verbatim).
 */
virtual bool certifying() const {
  return false;
}

/**
 * @brief Whether @p v is an independent literal for certification
 *        purposes: a base Bernoulli variable (or a constant), so that
 *        distinct literals have disjoint supports and an AND over them
 *        is decomposable.
 *
 * Only consulted when @c certifying() is @c true.
 *
 * @return @c false by default.
 */
virtual bool independent_literal(const value_type &) const {
  return false;
}

/**
 * @brief Build one complete world term: the conjunction of the
 *        @p present literals and the *negations* of the @p missing
 *        literals, certified decomposable.
 *
 * Only called when @c certifying() is @c true; the default throws.
 */
virtual value_type certified_world_term(
  const std::vector<value_type> &,
  const std::vector<value_type> &) const {
  throw SemiringException("This semiring does not certify enumerations.");
}

/**
 * @brief Build the disjunction of pairwise-exclusive @p disjuncts,
 *        certified deterministic.
 *
 * Only called when @c certifying() is @c true; the default throws.
 */
virtual value_type certified_exclusive_plus(
  const std::vector<value_type> &) const {
  throw SemiringException("This semiring does not certify enumerations.");
}
};


}

#endif /* SEMIRING_H */
