/**
 * @file AggMarginalEvaluator.h
 * @brief Exact closed-form HAVING @c COUNT(*) @c op @c C probability over
 *        *safe-join* lineage -- the recursive marginal-vector engine of the
 *        Ré-Suciu HAVING trichotomy (see @c doc/TODO/having-trichotomy.md,
 *        Gain 4 / Priority 4).
 *
 * @c CountCmpEvaluator (the flat Poisson-binomial pre-pass) is exact only
 * when the @c gate_agg children are pairwise leaf-disjoint, i.e. flat
 * single-table COUNT.  A join makes two aggregate-input rows share a base
 * tuple (fan-out, e.g. @c R(k,a),S(a,b) with several @c b per @c a), so the
 * contributors stop being independent Bernoulli trials and
 * @c CountCmpEvaluator bails to @c provsql_having's exponential enumeration.
 *
 * This pass generalises the flat case to the *single-level fan-out* shape:
 * each contributor is a product (conjunction) of @c gate_input leaves, and
 * the contributors partition into independent **blocks** that share a common
 * "root" leaf.  Per block the count distribution is the disjoint mixture
 * @f$(1-p_{\text{root}})\,\delta_0 + p_{\text{root}}\,\mathrm{PB}(\text{private})@f$
 * (the @c ⊥ combinator); independent blocks combine by **convolution** (the
 * @c ⊛^+ combinator).  The answer is @f$\sum_{c\ge 1,\;c\,\theta\,C} m[c]@f$
 * over the resulting count PMF @c m, with the empty group (@c c = 0) excluded
 * exactly as in @c CountCmpEvaluator / SQL @c HAVING semantics.
 *
 * **Soundness is circuit-only and self-gating.**  No planner-time skeleton
 * certificate is consulted yet: the pass fires only when the circuit exhibits
 * the clean factored shape (one leaf common to *every* member of a block, the
 * per-member private leaves pairwise disjoint, every involved leaf private to
 * the cmp subtree).  A non-hierarchical skeleton -- e.g. the triangle
 * @c R(x,y),S(y,z),T(z,x) -- has no leaf common to all members of its block,
 * so the structural check rejects it and the cmp falls through to exact
 * enumeration.  Multi-level nesting and shapes the circuit cannot self-certify
 * are the job of the later @c CERT_SAFE_AGG_PLAN planner certificate.
 *
 * Runs as a probability-side pre-pass in @c probability_evaluate.cpp, *after*
 * @c runCountCmpEvaluator (so the cheap flat path still wins where it
 * applies; this pass only ever sees the join-shaped cmps the flat pass left
 * behind), gated by the same @c provsql.cmp_probability_evaluation GUC and
 * sharing its sound-only contract (replace the @c gate_cmp by a Bernoulli
 * @c gate_input, meaningless to symbolic semirings).
 */
#ifndef PROVSQL_AGG_MARGINAL_EVALUATOR_H
#define PROVSQL_AGG_MARGINAL_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the safe-join COUNT marginal-vector pre-pass over @p gc.
 *
 * For every @c gate_cmp matching the single-level fan-out COUNT shape (see
 * file docstring) computes the comparator's exact probability by block
 * mixture + convolution and replaces the cmp by a Bernoulli @c gate_input via
 * @c GenericCircuit::resolveCmpToBernoulli.  Leaves every other cmp untouched.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runAggMarginalCountEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_AGG_MARGINAL_EVALUATOR_H
