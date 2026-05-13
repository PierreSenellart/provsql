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
 * The term @e peephole comes from compiler engineering (McKeeman, CACM
 * 8(7), 1965): a small sliding window over consecutive instructions /
 * gates, a fixed list of local pattern -> replacement rules, iterated
 * to a fixed point.  Each rule below looks at one @c gate_arith plus
 * its immediate children, never further, which is exactly the
 * peephole scope.  Contrast with @c RangeCheck (global interval-
 * propagation walk) and @c runHybridDecomposer (union-find over
 * base-RV footprints across the whole circuit).
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
 * - Mixture lift: a @c PLUS / @c TIMES over a single @c gate_mixture
 *   child pushes the other wires inside the branches.  For the classic
 *   3-wire shape <tt>mixture(p, X, Y)</tt> each branch becomes a fresh
 *   arith over the lifted wires; for the categorical N-wire shape
 *   <tt>mixture(key, mul_1, ..., mul_n)</tt> the constant offset / factor
 *   is folded directly into each mulinput's value text (sharing the key
 *   keeps the new mixture correlated with the original).
 * - PLUS coefficient aggregation: a @c PLUS whose wires decompose as
 *   <tt>a_i·Z_i + b_i</tt> merges same-@c Z_i terms by summing
 *   coefficients (so @c X+X folds to @c 2·X, @c X-X to @c 0, etc.) and
 *   consolidates the constant offsets.
 * - Scalar-times-RV closure: <tt>c·X</tt> for @c X a @c gate_rv whose
 *   distribution admits a closed-form scale (Normal, Uniform, Exp,
 *   Erlang) folds to a single scaled @c gate_rv.
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
 * The simplifier runs in two passes for shared-RV identity preservation:
 * pass 1 applies every rule EXCEPT the scalar-times-RV closure so the
 * aggregator gets to see @c c·X-shaped wires inside a parent PLUS with
 * their underlying RV identity intact (otherwise a bottom-up fold of
 * the inner TIMES would mint a fresh @c gate_rv there and decouple it
 * from a sibling reference to the same @c X).  Pass 2 then folds the
 * remaining @c TIMES wrappers with the scalar-times-RV rule.
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
 * @brief Constant-fold pass over every @c gate_arith in @p gc.
 *
 * Walks the circuit bottom-up and replaces any @c gate_arith whose
 * children all evaluate to scalar constants with the equivalent
 * @c gate_value (e.g. @c arith(NEG, value:2) becomes @c value:-2,
 * @c arith(PLUS, value:1, value:2) becomes @c value:3).
 *
 * Strictly a subset of @c runHybridSimplifier (only @c try_eval_constant
 * fires; no family closures, identity drops, or mixture lifts), and
 * therefore safe to run at load time alongside @c runRangeCheck and
 * @c foldSemiringIdentities: the resulting @c gate_value gates carry
 * no random identity, so no consumer's shared-RV coupling is broken
 * by the rewrite.  The family closures stay behind the separate
 * @c hybrid_evaluation GUC because they replace a multi-leaf subtree
 * with a fresh @c gate_rv UUID and would decouple shared base RVs
 * that other parts of the circuit reference.
 *
 * Lifts the @c -c::random_variable parser quirk (which builds an
 * @c arith(NEG, value:c) gate rather than @c value:-c) into a clean
 * @c gate_value before downstream consumers like
 * @c collectRvConstraints / @c asRvVsConstCmp inspect the circuit.
 *
 * @return Number of gates rewritten.
 */
unsigned runConstantFold(GenericCircuit &gc);

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
 * **Singleton groups** are marginalised into a single
 * @c gate_input via @c GenericCircuit::resolveCmpToBernoulli.
 *
 * **Multi-cmp shared-island groups** (k comparators sharing one or
 * more base @c gate_rv leaves, detected via pairwise footprint
 * overlap with union-find) are resolved by inlining a 2^k joint
 * distribution table:
 * - One anonymous @c gate_input acts as the block key.
 * - One @c gate_mulinput per joint outcome with positive probability,
 *   all sharing the key, carries the joint mass (mutually-exclusive
 *   block).
 * - Each comparator is rewritten as @c gate_plus over the mulinputs
 *   whose joint outcome word has the comparator's bit set.
 * The downstream OR over the rewritten comparators thereby observes
 * the dependent joint distribution: mulinputs across comparators
 * dedup at OR sites in
 * @c BooleanCircuit::independentEvaluationInternal (or are
 * Bayesian-tree-rewritten by @c rewriteMultivaluedGates before
 * @c tree-decomposition / @c monte-carlo / external compilers).
 * Groups with k > @c JOINT_TABLE_K_MAX (currently 8, i.e. 256
 * outcomes) fall through to whole-circuit MC to keep the
 * materialisation bounded.
 *
 * @param gc       Circuit to mutate in place.
 * @param samples  Number of MC iterations used per marginalisation.
 *                 Callers typically pass @c provsql_rv_mc_samples.
 * @return         Number of comparators resolved by this pass.
 */
unsigned runHybridDecomposer(GenericCircuit &gc, unsigned samples);

}  // namespace provsql

#endif  // PROVSQL_HYBRID_EVALUATOR_H
