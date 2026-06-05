/**
 * @file SumCmpEvaluator.h
 * @brief Closed-form probability resolution for HAVING
 *        @c SUM(a) @c op @c C @c gate_cmps via a weighted-sum DP.
 *
 * For comparators that reduce to
 * @c gate_cmp(gate_agg(SUM, semimod_i(K_i, m_i)*), gate_value(C)) where
 * the contributors are mutually independent Bernoulli trials (each K_i a
 * private read-once sub-circuit, the same soundness contract as
 * @c CountCmpEvaluator), the running sum @c S = sum of the present rows'
 * integer weights @c m_i is a weighted Poisson-binomial.  Its full
 * distribution @c dp[s] = Pr(S = s) is computed by a subset-sum
 * convolution -- @c dp'[s] = dp[s](1 - p_i) + dp[s - m_i] p_i per child --
 * over the reachable range @c [sum of negative m_i, sum of positive m_i].
 * The comparator's probability is then @c sum_{s : s op C} dp[s], with the
 * empty-group world subtracted (see below).  Cost @c O(N x R) where
 * @c R is the reachable-sum range.
 *
 * **Pseudo-polynomial caveat.**  @c R is linear in the magnitude of the
 * weights and the threshold, i.e. exponential in their bit-length, so
 * this DP is only pseudo-polynomial (unlike COUNT, where the analogous
 * range is bounded by @c N).  The pass therefore declines (leaving the
 * cmp for @c provsql_having) when @c R exceeds @c kMaxSumRange, falling
 * back to the general enumeration path.
 *
 * Soundness condition (checked per cmp gate, identical to
 * @c CountCmpEvaluator) :
 *  - shape matches `gate_cmp(gate_agg(SUM, semimod_i(K_i, m_i)*), gate_value(C))`
 *    in either order ;
 *  - the aggregate is consumed by this cmp alone, each semimod by the
 *    aggregate alone, and every randomness-bearing gate inside each K_i
 *    has reference count 1 -- so the contributors are pairwise
 *    leaf-disjoint, private to the cmp, and individually read-once.
 *
 * Empty-group semantics mirror @c count_enum / @c CountCmpEvaluator : the
 * all-absent world (the only world with no present row) never satisfies
 * HAVING.  Its sum is the empty sum @c 0, so when @c 0 @c op @c C holds
 * its mass @c prod_i (1 - p_i) is subtracted from the answer.  Worlds
 * that are non-empty but happen to sum to @c 0 (a present @c m_i = 0, or
 * cancelling signed weights) are legitimately kept.
 *
 * After replacement the cmp is a Bernoulli @c gate_input carrying the
 * numeric probability, semantically meaningful only on the probability
 * path.  Like @c runCountCmpEvaluator, the pass runs inside
 * @c probability_evaluate.cpp, gated on
 * @c provsql.cmp_probability_evaluation, not at load time.
 */
#ifndef PROVSQL_SUM_CMP_EVALUATOR_H
#define PROVSQL_SUM_CMP_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the weighted-sum DP pre-pass over @p gc.
 *
 * For every @c gate_cmp whose shape matches a SUM(a) op C HAVING
 * predicate over independent private contributors and whose reachable-sum
 * range is within @c kMaxSumRange, computes the comparator's probability
 * by the subset-sum DP and replaces the cmp by a Bernoulli @c gate_input
 * via @c GenericCircuit::resolveCmpToBernoulli.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runSumCmpEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_SUM_CMP_EVALUATOR_H
