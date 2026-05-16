/**
 * @file CountCmpEvaluator.h
 * @brief Closed-form Poisson-binomial CDF resolution for HAVING
 *        @c COUNT(*) @c op @c C @c gate_cmps.
 *
 * For comparators that reduce to
 * @c gate_cmp(gate_agg(COUNT, semimod children), gate_value(C)) where
 * each semimod child's K side is a distinct single @c gate_input
 * leaf (the first-slice scope: flat-table HAVING COUNT queries), the
 * comparator's probability is @c Pr(B op C) with @c B Poisson-binomial
 * over the children's stored marginal probabilities.  A partial DP
 * computed on the smaller side of @c C (either the lower tail
 * @c [0, C-1] on the original Bernoullis, or the upper tail computed
 * via the complementary @c [0, N-C] on the inverted Bernoullis
 * @c Y_i @c = @c 1-X_i) gives the answer in @c O(N x min(C, N-C))
 * time per cmp, without ever materialising the @c binom(N, C) DNF
 * that @c provsql_having's @c enumerate_valid_worlds path would
 * otherwise emit.
 *
 * Soundness condition (checked per cmp gate) :
 *  - shape matches `gate_cmp(gate_agg(COUNT, semimod_i(K_i, 1)*), gate_value(C))`
 *    in either order ;
 *  - every K_i is a single @c gate_input leaf (no @c gate_times,
 *    no @c gate_mulinput, no nested arithmetic) ;
 *  - the K_i leaves are pairwise distinct (no shared input across
 *    children) ;
 *  - none of the K_i leaves is reachable from the rest of the
 *    surrounding circuit, i.e. each input appears exactly once and
 *    only inside this cmp's subtree.
 *
 * The first two conditions are cheap to check locally.  The third
 * (no outside reachability) is checked once per pass via a
 * reference-count walk over the whole circuit.
 *
 * After replacement the cmp is a Bernoulli @c gate_input carrying
 * the numeric probability, semantically meaningful only on the
 * probability path.  Like @c runAnalyticEvaluator, the pass runs
 * inside @c probability_evaluate.cpp, not at load time.
 *
 * Empty-group semantics mirror @c count_enum and SQL : worlds with
 * zero present children are excluded.  In particular the
 * "all-absent" mass @c dp[N][0] is never included regardless of
 * operator.
 */
#ifndef PROVSQL_COUNT_CMP_EVALUATOR_H
#define PROVSQL_COUNT_CMP_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the Poisson-binomial pre-pass over @p gc.
 *
 * For every @c gate_cmp whose shape matches the first-slice scope
 * (see file docstring), computes the comparator's probability by
 * Poisson-binomial CDF and replaces the cmp by a Bernoulli
 * @c gate_input via @c GenericCircuit::resolveCmpToBernoulli.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runCountCmpEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_COUNT_CMP_EVALUATOR_H
