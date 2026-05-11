/**
 * @file HybridEvaluator.h
 * @brief Peephole simplifier for continuous @c gate_arith sub-circuits.
 *
 * The hybrid evaluator owns the simplification + island-decomposition
 * passes invoked between the universal cmp-resolution (@c RangeCheck)
 * and the probability-specific analytic-CDF resolution
 * (@c AnalyticEvaluator).  This header exposes the simplifier; the
 * island decomposer will be added alongside in the same namespace.
 *
 * The simplifier rewrites @c gate_arith gates in-place using closure
 * rules that preserve the gate's scalar value in every world:
 * - Constant folding of @c gate_arith subtrees over only @c gate_value
 *   leaves &mdash; collapses to a single @c gate_value.
 * - Multiplicative zero short-circuit: @c TIMES with any constant-zero
 *   wire becomes @c gate_value:0, regardless of the other (possibly
 *   non-constant) wires.
 * - Identity-element drops: remove @c gate_value:0 wires from a
 *   @c PLUS gate; remove @c gate_value:1 wires from a @c TIMES gate.
 * - Normal-family closure: a @c PLUS over linear combinations of
 *   independent normal @c gate_rv leaves folds to a single normal
 *   @c gate_rv whose mean and variance are the closed-form
 *   combinations.  The independence test mirrors @c Expectation's
 *   footprint check: each contributing normal must be reached through
 *   a disjoint base-RV UUID.
 * - Erlang-family closure: a @c PLUS over k ≥ 2 i.i.d. exponential
 *   @c gate_rv leaves with the same rate λ and pairwise-distinct
 *   UUIDs folds to a single Erlang(k, λ) @c gate_rv.
 *
 * The rules form a fixed-point loop per gate: after any rewrite
 * succeeds, the gate is re-evaluated under all rules until none fire.
 * Compositions like <tt>arith(NEG, arith(PLUS, value, value))</tt>
 * therefore collapse fully in one bottom-up pass.
 *
 * Soundness: every rule produces a circuit semantically equivalent to
 * the original in every world (same scalar distribution, same support,
 * same comparator outcomes).  The closures merge independent leaves
 * into a new leaf with a fresh distribution; downstream consumers
 * (sampler, RangeCheck, AnalyticEvaluator, Expectation) see the new
 * leaf and use the closed-form distribution machinery as if the user
 * had written it directly.
 *
 * Operates on the in-memory @c GenericCircuit only; the persistent
 * mmap store is never mutated.  Children that become orphaned by a
 * rewrite are not reaped here.
 *
 * Gated by the @c provsql.hybrid_evaluation GUC (default on):
 * @c probability_evaluate skips the pass entirely when the GUC is off,
 * letting users verify that the analytic path and the whole-circuit
 * MC fallback agree on the same query.
 */
#ifndef PROVSQL_HYBRID_EVALUATOR_H
#define PROVSQL_HYBRID_EVALUATOR_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Run the peephole simplifier over @p gc.
 *
 * Visits every gate in post-order and applies the closure rules
 * described in the header comment until a fixed point is reached.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of gate rewrites performed by this pass.
 */
unsigned runHybridSimplifier(GenericCircuit &gc);

/**
 * @brief Marginalise unresolved continuous-island @c gate_cmp gates
 *        into Bernoulli @c gate_input leaves.
 *
 * Runs after @c RangeCheck, the simplifier (@c runHybridSimplifier),
 * and @c AnalyticEvaluator have done what they can.  Picks up the
 * residual comparators whose two sides are an entirely continuous
 * island (subtree of @c gate_value, @c gate_rv, @c gate_arith with
 * no Boolean structure underneath) but whose specific shape is not
 * one the analytic CDF resolver handles &mdash; e.g.
 * <tt>Normal + Uniform &gt; 0</tt>, heterogeneous-rate sums of
 * exponentials, or other compositions the simplifier could not fold
 * to a bare distribution leaf.
 *
 * Each qualifying comparator is marginalised by drawing @p samples
 * worlds and applying the comparator scalar-by-scalar; the empirical
 * probability replaces the @c gate_cmp via
 * @c resolveCmpToBernoulli.  The circuit downstream becomes purely
 * Boolean, so the existing @c independent / @c tree-decomposition /
 * compilation methods become available on circuits that would
 * otherwise have to fall through to whole-circuit MC.
 *
 * **Single-cmp islands only.**  When two or more comparators share
 * a base @c gate_rv (their per-cmp footprints overlap), their
 * joint distribution would have to be enumerated together;
 * marginalising them independently would silently introduce
 * spurious-independence error.  This pass detects shared-island
 * groups via the base-RV footprint and skips them: those cmps
 * stay as @c gate_cmp and fall through to whole-circuit MC.
 * Resolving the shared-island case is the second half of Priority
 * 7(b) (the 2^k joint-table construction).
 *
 * @param gc       Circuit to mutate in place.
 * @param samples  Number of MC iterations used per marginalisation.
 *                 Callers typically pass @c provsql_rv_mc_samples.
 * @return         Number of comparators resolved by this pass.
 */
unsigned runHybridDecomposer(GenericCircuit &gc, unsigned samples);

}  // namespace provsql

#endif  // PROVSQL_HYBRID_EVALUATOR_H
