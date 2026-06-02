/**
 * @file AggMarginalEvaluator.h
 * @brief Exact closed-form HAVING @c COUNT(*) @c op @c C probability over
 *        *safe-join* lineage -- the recursive marginal-vector engine of the
 *        Ré-Suciu HAVING trichotomy (see
 *        @c doc/source/dev/probability-evaluation.rst).
 *
 * @c CountCmpEvaluator (the flat Poisson-binomial pre-pass) is exact only
 * when the @c gate_agg children are pairwise leaf-disjoint, i.e. flat
 * single-table COUNT.  A join makes two aggregate-input rows share a base
 * tuple (fan-out, e.g. @c R(k,a),S(a,b) with several @c b per @c a), so the
 * contributors stop being independent Bernoulli trials and
 * @c CountCmpEvaluator bails to @c provsql_having's exponential enumeration.
 *
 * This pass generalises the flat case to any *hierarchical* (laminar) join:
 * each contributor is a product (conjunction) of @c gate_input leaves
 * (nested @c gate_times from SPJ subqueries / views is flattened, since
 * @c times is AND on the probability path, so detection is invariant to
 * join order and subquery nesting), and the count distribution is computed
 * **recursively** down the hierarchy.  At
 * each level the contributors partition into independent **blocks** (by shared
 * leaf); a block factors out the leaves common to *every* member (this level's
 * shared "root" event) and the block count is the disjoint mixture
 * @f$(1-p_{\text{root}})\,\delta_0 + p_{\text{root}}\,m_{\text{inner}}@f$ (the
 * @c ⊥ combinator), where @f$m_{\text{inner}}@f$ is the same construction
 * applied to the per-member residual leaf sets one level deeper; independent
 * blocks combine by **convolution** (the @c ⊛^+ combinator).  Single-level
 * fan-out (@c R(k,a),S(a,b)) is the case where every residual is one leaf and
 * @f$m_{\text{inner}}@f$ is a Poisson-binomial; deeper nesting (orders→items
 * under a user) recurses further.  The answer is
 * @f$\sum_{c\ge 1,\;c\,\theta\,C} m[c]@f$ over the resulting count PMF @c m,
 * with the empty group (@c c = 0) excluded exactly as in
 * @c CountCmpEvaluator / SQL @c HAVING semantics.
 *
 * **Soundness is circuit-only and self-gating.**  No planner-time skeleton
 * certificate is consulted: the recursion is exact iff at every level each
 * multi-member block has a leaf common to *all* its members (and every
 * involved leaf is private to the cmp subtree).  A non-laminar shape -- e.g.
 * the triangle @c R(x,y),S(y,z),T(z,x) -- produces a multi-member block with
 * no common leaf at some level, which clears the soundness flag and falls the
 * cmp through to exact enumeration.  This circuit-only recursion already
 * covers the tuple-independent hierarchical class at any depth; a planner
 * @c CERT_SAFE_AGG_PLAN certificate is still needed only for shapes the
 * circuit cannot self-certify (e.g. BID disjoint-block @c ⊥ structure).
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
 * @brief Run the safe-join aggregate marginal-vector pre-pass over @p gc.
 *
 * For every @c gate_cmp matching the hierarchical-join shape (see file
 * docstring) over @c COUNT / @c SUM / @c MIN / @c MAX, computes the
 * comparator's exact probability through the recursive hierarchical engine
 * and replaces the cmp by a Bernoulli @c gate_input via
 * @c GenericCircuit::resolveCmpToBernoulli.  Leaves every other cmp
 * untouched.  COUNT / SUM use the count/weighted-sum distribution
 * (block mixture + additive convolution); MIN / MAX reduce to a handful of
 * "all of a value-thresholded subset absent" probabilities over the same
 * hierarchical recursion.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runAggMarginalEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_AGG_MARGINAL_EVALUATOR_H
