/**
 * @file CmpEvaluatorCommon.h
 * @brief Shared machinery for the closed-form HAVING @c gate_cmp
 *        probability evaluators (Poisson-binomial COUNT, MIN / MAX, …).
 *
 * Every closed-form HAVING evaluator that runs as a probability-side
 * pre-pass (see @c probability_evaluate.cpp) shares three concerns:
 *
 *  - matching the canonical shape
 *    @c gate_cmp(gate_agg(α, semimod_i(K_i, m_i)*), gate_value(C))
 *    in either operand order (@c matchAggCmp) ;
 *  - certifying that the @c gate_agg children are mutually independent
 *    Bernoulli contributors with leaves private to the cmp's subtree,
 *    via the reference-count walk (@c computeRefCounts) ;
 *  - computing each contributor's read-once marginal probability
 *    (@c contributorProb).
 *
 * These were first written for @c CountCmpEvaluator; they are factored
 * here so the MIN / MAX (and future SUM) evaluators reuse the exact same
 * soundness contract rather than re-deriving it.  See
 * @c CountCmpEvaluator.h for the soundness argument in full.
 */
#ifndef PROVSQL_CMP_EVALUATOR_COMMON_H
#define PROVSQL_CMP_EVALUATOR_COMMON_H

#include <vector>

#include "Aggregation.h"     // AggregationOperator + ComparisonOperator
#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Result of matching a @c gate_cmp against the canonical HAVING
 *        aggregate-comparison shape.
 *
 * Populated by @c matchAggCmp.  The per-evaluator soundness checks (ref
 * counts on @c agg / @c semimods, read-once marginals over @c ks) are
 * left to the caller; this struct only carries the syntactic match.
 */
struct AggCmpMatch {
  gate_t agg{};                    ///< the @c gate_agg operand of the cmp
  std::vector<gate_t> semimods;    ///< the per-child @c gate_semimod parents
  std::vector<gate_t> ks;          ///< the K side of each semimod (contributor root)
  std::vector<long> ms;            ///< the M side of each semimod (per-row value), scaled to a common integer grid (numeric / decimal-float domains; see @c matchAggCmp)
  AggregationOperator agg_kind{};  ///< effective aggregate (SUM-of-1s remapped to COUNT)
  ComparisonOperator op{};         ///< comparator, flipped if the agg sits on the right
  long C{};                        ///< the constant threshold, on the same integer grid as @c ms
};

/**
 * @brief Try to match @p cmp against
 *        @c gate_cmp(gate_agg(α, semimod_i(K_i, m_i)*), gate_value(C)).
 *
 * Accepts both operand orders (agg left or right), flipping @c op in the
 * latter case.  Mirrors @c pw_from_cmp_gate's @c build_from and the
 * SUM-of-1s → COUNT remap.  Returns @c false (leaving @p out untouched)
 * on any shape mismatch; cheap to call.
 *
 * @param[in]  gc   Circuit to inspect.
 * @param[in]  cmp  Candidate @c gate_cmp.
 * @param[out] out  Filled on success.
 * @return     @c true iff the shape matched.
 */
bool matchAggCmp(GenericCircuit &gc, gate_t cmp, AggCmpMatch &out);

/**
 * @brief Reference count of every gate as a wire-target across the whole
 *        circuit.  One pass over all wire lists; @c O(total wires).
 */
std::vector<unsigned> computeRefCounts(const GenericCircuit &gc);

/**
 * @brief Read-once marginal probability of a count/aggregate contributor
 *        (the K side of a semimod).
 *
 * Exact precisely when the contributor's sub-circuit is a private
 * read-once tree: every randomness-bearing gate it visits must have
 * reference count 1.  That single condition gives pairwise-disjoint leaf
 * sets across contributors, no reuse outside the cmp, and read-once-ness
 * within a contributor.  Supports @c input / @c times / @c plus /
 * @c monus and the @c one / @c zero constants; clears @p ok on any other
 * gate type or on a reference-count violation.  See
 * @c CountCmpEvaluator.h for the full argument.
 */
double contributorProb(const GenericCircuit &gc, gate_t g,
                       const std::vector<unsigned> &ref, bool &ok);

}  // namespace provsql

#endif  // PROVSQL_CMP_EVALUATOR_COMMON_H
