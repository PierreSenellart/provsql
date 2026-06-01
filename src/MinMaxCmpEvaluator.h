/**
 * @file MinMaxCmpEvaluator.h
 * @brief Closed-form probability resolution for HAVING
 *        @c MIN(a) @c op @c C and @c MAX(a) @c op @c C @c gate_cmps.
 *
 * For comparators that reduce to
 * @c gate_cmp(gate_agg(MIN|MAX, semimod_i(K_i, m_i)*), gate_value(C))
 * where the contributors are mutually independent Bernoulli trials
 * (each K_i a private read-once sub-circuit, the same soundness contract
 * as @c CountCmpEvaluator), the comparator's probability has a trivial
 * closed form -- products of @c (1 - p_i) over the children partitioned
 * on @c m_i vs @c C, with no DP and no DNF.
 *
 * SQL HAVING semantics exclude the empty group (a group with no present
 * row never appears in the answer), exactly as @c CountCmpEvaluator and
 * @c count_enum do.  Writing @c m_i for the per-row value of the
 * aggregated column, @c G for "good" children and @c B for "bad"
 * children, and @c qprod(S) = ∏_{i∈S}(1 - p_i), the twelve cases are:
 *
 *   MAX ≥ C : G={m_i≥C}            1 − qprod(G)
 *   MAX > C : G={m_i>C}            1 − qprod(G)
 *   MAX ≤ C : B={m_i>C}, G={m_i≤C} qprod(B)·(1 − qprod(G))
 *   MAX < C : B={m_i≥C}, G={m_i<C} qprod(B)·(1 − qprod(G))
 *   MAX = C : B={m_i>C}, E={m_i=C} qprod(B)·(1 − qprod(E))
 *   MAX ≠ C :                      (1 − qprod(all)) − Pr(MAX = C)
 *   MIN ≤ C : G={m_i≤C}            1 − qprod(G)
 *   MIN < C : G={m_i<C}            1 − qprod(G)
 *   MIN ≥ C : B={m_i<C}, G={m_i≥C} qprod(B)·(1 − qprod(G))
 *   MIN > C : B={m_i≤C}, G={m_i>C} qprod(B)·(1 − qprod(G))
 *   MIN = C : B={m_i<C}, E={m_i=C} qprod(B)·(1 − qprod(E))
 *   MIN ≠ C :                      (1 − qprod(all)) − Pr(MIN = C)
 *
 * (The two "good" and "bad" sets are disjoint and independent, so the
 * two factors multiply; the @c (1 − qprod(G)) factor enforces group
 * non-emptiness.)
 *
 * After replacement the cmp is a Bernoulli @c gate_input carrying the
 * numeric probability, semantically meaningful only on the probability
 * path.  Like @c runCountCmpEvaluator and @c runAnalyticEvaluator, the
 * pass runs inside @c probability_evaluate.cpp, gated on
 * @c provsql.cmp_probability_evaluation, not at load time.
 */
#ifndef PROVSQL_MIN_MAX_CMP_EVALUATOR_H
#define PROVSQL_MIN_MAX_CMP_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the MIN / MAX closed-form pre-pass over @p gc.
 *
 * For every @c gate_cmp whose shape matches a MIN(a)/MAX(a) op C HAVING
 * predicate over independent private contributors, computes the
 * comparator's probability in closed form and replaces the cmp by a
 * Bernoulli @c gate_input via @c GenericCircuit::resolveCmpToBernoulli.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparators resolved by this pass.
 */
unsigned runMinMaxCmpEvaluator(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_MIN_MAX_CMP_EVALUATOR_H
