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
 * A multi-member block with *no* common leaf is the **join (Cartesian
 * product)** node @c R(a),S(a,b),T(a,c): the contributors are the complete
 * product of per-factor parts, so @c count is the product of the per-factor
 * counts.  A @c SUM whose value lives on one factor is @f$S_f\cdot M@f$ (that
 * factor's weighted sum times the others' count product); a *branch-spanning*
 * value that is **additively separable** across factors (e.g. @c sum(b+c)) is
 * @f$\sum_f S_f\cdot\prod_{g\ne f} N_g@f$, folded exactly from the per-factor
 * *joint* @c (sum,count) distributions (@c sumCountPMF); a
 * **multiplicatively separable** value (e.g. @c sum(b*c)) is
 * @f$\prod_f S_f@f$, the product of the per-factor weighted sums
 * (@c mulSeparableSumPMF).  A value that is neither (e.g. @c sum(b*c+b+c))
 * couples the factors and self-gates back to enumeration.
 *
 * **Soundness is circuit-only and self-gating.**  No planner-time skeleton
 * certificate is consulted: the recursion is exact iff at every level each
 * multi-member block has a leaf common to *all* its members (and every
 * involved leaf is private to the cmp subtree).  A non-laminar shape -- e.g.
 * the triangle @c R(x,y),S(y,z),T(z,x) -- produces a multi-member block with
 * no common leaf at some level, which clears the soundness flag and falls the
 * cmp through to exact enumeration.  This circuit-only recursion covers the
 * tuple-independent hierarchical class at any depth.
 *
 * **BID blocks (@c COUNT / @c SUM / @c AVG / @c MIN / @c MAX).**  A
 * @c repair_key block surfaces in the circuit as a set of @c gate_mulinput
 * contributors sharing a block-key child, mutually exclusive with
 * per-alternative probabilities.  Such a contributor is recognised and the
 * block handled as a *categorical* (at most one alternative present, the null
 * arm Σp_i<1 contributing 0), independent of the TID part and of other blocks:
 * @c COUNT / @c SUM / @c AVG convolve the block's count / weighted-sum
 * distribution, and @c MIN / @c MAX fold a per-block factor
 * (1-Σ_{pred} p_alt) into each @c pAllAbsent over a value-thresholded subset.
 * No planner certificate is needed, since the block *is* visible in the
 * circuit.  The residual genuinely certificate-only case is a declared key on a
 * plain TID table (mutual exclusion in @c block_key metadata only, no
 * @c mulinput): that would need a planner @c CERT_SAFE_AGG_PLAN.
 *
 * **Non-read-once UNION / EXCEPT contributors.**  A @c UNION / @c EXCEPT over a
 * join that re-uses a base tuple gives a contributor that is a @c gate_plus /
 * @c gate_monus repeating the shared leaf -- @c (r∧s)∨(r∧t) or @c (r∧s)∖(r∧t)
 * -- which @c contributorProb (read-once only) rejects.  When the contributor's
 * footprint is *private* (so it is independent of every other contributor),
 * @c contributorExactMarginal computes its exact marginal by brute force over
 * its private leaves (the internal sharing resolved exactly) and the
 * contributor is modelled as a one-alternative BID block -- an independent
 * event -- reusing the categorical machinery above.  A base tuple shared
 * *across* contributors of the same group is genuinely @f$\#P@f$-hard and bails.
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
