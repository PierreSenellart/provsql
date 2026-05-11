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

}  // namespace provsql

#endif  // PROVSQL_HYBRID_EVALUATOR_H
