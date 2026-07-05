/**
 * @file ComparatorResolution.h
 * @brief The single comparator-resolution pipeline and the single
 *        Boolean-subcircuit probability entry point, shared by the
 *        probability path and the scalar-moment evaluator.
 *
 * Historically the probability path (@c probability_evaluate) and the
 * moment evaluator (@c Expectation.cpp) each had their own, divergent
 * notion of "resolve the RV comparators, then compute the probability of
 * a Boolean function".  This header collapses both into one place.
 */
#ifndef COMPARATOR_RESOLUTION_H
#define COMPARATOR_RESOLUTION_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the comparator-resolution pipeline on @p gc, rewriting every
 *        @c gate_cmp (RV comparison, HAVING aggregate comparison) into
 *        Boolean structure so the downstream @c getBooleanCircuit /
 *        BoolExpr translation never meets a raw comparator.
 *
 * The single source of truth for the resolution pipeline.  Two flags carry
 * the (principled) differences between the two callers:
 *
 * @param simplify  run @c runHybridSimplifier, which folds
 *   @c gate_arith-over-@c gate_rv into closed-form distributions.  Sound for
 *   the probability path (the circuit collapses to Boolean, so a folded
 *   value RV is never observed as a value) but it MUST be false for the
 *   scalar-moment path: that path takes moments over the very value
 *   structure the fold rewrites, and a value-structure shared latent
 *   (@c mu_i + d inside @c normal(mu_i+d, s)) would be decoupled into
 *   independent Normals, corrupting the covariance.
 * @param decompose run @c runHybridDecomposer, which groups correlated
 *   comparators into a @c gate_mulinput joint table (capped at
 *   @c JOINT_TABLE_K_MAX).  The probability path wants it; the moment path
 *   leaves it false so genuinely-correlated comparators stay raw and fall
 *   to the direct RV Monte-Carlo sampler, which couples the shared base RV
 *   at any group size (no k cap, no joint-table quantisation).
 *
 * Emits the "shortcut by probability-side pre-pass" NOTICE at
 * @c provsql.verbose_level >= 5.
 */
void resolveComparators(GenericCircuit &gc, gate_t root,
                        bool simplify, bool decompose);

/**
 * @brief Probability of the Boolean function rooted at @p root in @p gc,
 *        via the one central dispatch.
 *
 * @c getBooleanCircuit builds the Boolean view (HAVING semantics + BoolExpr
 * translation); @c MethodCatalog::chooseAndRun then runs the cost-ordered
 * exact/approximate portfolio (independent / tree-decomposition /
 * compilation).  If the Boolean translation rejects a residual raw RV
 * @c gate_cmp (a comparator neither @c resolveComparators nor the closed
 * forms could eliminate), the probability is estimated by Monte-Carlo over
 * the base RVs (@c monteCarloRV) instead of failing.  This is THE entry
 * point every caller that needs "the probability of a Boolean subcircuit"
 * routes through -- the top-level @c probability_evaluate and the moment
 * evaluator's mixture weights alike -- so the method portfolio is
 * single-sourced.  @p gc's comparators should already be resolved (see
 * @c resolveComparators) for an exact result; the MC fallback covers what
 * remains.
 */
double booleanSubcircuitProbability(GenericCircuit &gc, gate_t root);

}  // namespace provsql

#endif /* COMPARATOR_RESOLUTION_H */
