/**
 * @file DTree.h
 * @brief Anytime interval-bounds probability for monotone DNFs (d-trees).
 *
 * Implements the recursive half of Olteanu, Huang & Koch, "Approximate
 * Confidence Computation in Probabilistic Databases" (ICDE 2010): the cheap
 * leaf bound @c BooleanCircuit::dnfBounds is refined by two decompositions --
 * independent-or (connected components of the clause graph) and Shannon
 * expansion (on the most frequent variable) -- until the certified interval is
 * narrow enough, or exactly (interval width 0).
 *
 * The recursion operates purely on the @e clause-support sets of a monotone
 * DNF: a clause is the conjunction of its positive input leaves, so the DNF is
 * the list of those leaf sets, and every decomposition (component split, Shannon
 * cofactor) maps a DNF to DNFs of the same representation.
 */
#ifndef DTREE_H
#define DTREE_H

#include <set>
#include <vector>

#include "Circuit.h" // gate_t

class BooleanCircuit;

namespace provsql {

/// A certified probability interval @c lower <= Pr <= upper.
struct DTreeInterval {
  double lower;
  double upper;
};

/**
 * @brief Certified probability interval of a monotone DNF, refined to a target
 *        width (Olteanu-Huang-Koch d-tree).
 *
 * Refines @c BooleanCircuit::dnfBounds by independent-or decomposition and
 * Shannon expansion until @c upper-lower <= @p max_width, propagating the width
 * budget so the returned interval honours it: through Shannon the same budget
 * passes to both cofactors (the mixture width is at most the larger branch's),
 * through an independent-or of @c k components each gets @c max_width/k (the OR
 * width is at most the sum of the component widths).  @p max_width == 0 forces
 * exact compilation (@c lower == upper).
 *
 * @param c          Circuit owning the input marginals (@c getProb).
 * @param clauses    The DNF as per-clause input-leaf supports (consumed).
 * @param max_width  Absolute target for @c upper-lower (0 = exact).
 * @return           A sound interval with @c lower <= Pr[clauses] <= upper.
 */
DTreeInterval dtreeBounds(const BooleanCircuit &c,
                          std::vector<std::set<gate_t> > clauses,
                          double max_width, unsigned long budget = 0,
                          unsigned long *steps_out = nullptr);

/**
 * @brief Certified probability interval of an @e arbitrary Boolean circuit,
 *        refined to a target width (the d-tree generalised off monotone DNF).
 *
 * Same anytime engine as @c dtreeBounds, but recursing on the circuit DAG
 * (@c AND / @c OR / @c NOT / @c IN) instead of a flat monotone-DNF clause set,
 * so it applies to negation (@c EXCEPT / @c monus, encoded @c A AND NOT B),
 * nested @c AND / @c OR (e.g. a CNF-shaped circuit), and arbitrary sharing.
 *
 * The cheap leaf bound generalises @c dnfBounds soundly to any gate: an
 * independent-component split (children with disjoint free-variable footprints
 * compose exactly), then a Bonferroni lower / min upper for @c AND, a max lower
 * / union upper for @c OR, and a flip @c [1-U,1-L] for @c NOT.  It is refined by
 * independent-component decomposition and Shannon expansion on the most frequent
 * shared free variable until @c upper-lower <= @p max_width (0 = exact).  Every
 * step keeps @c lower <= Pr <= upper (Shannon is an exact mixture; independence
 * is over disjoint input cones, never overclaimed).
 *
 * Throws @c CircuitException on a multivalued (@c MULIN / @c MULVAR) or
 * @c UNDETERMINED gate in the cone of @p root, so the caller falls back to
 * another method on BID circuits.
 *
 * @param c          Circuit (gate types, wiring, input marginals).
 * @param root       Root gate whose probability interval is computed.
 * @param max_width  Absolute target for @c upper-lower (0 = exact).
 * @return           A sound interval with @c lower <= Pr[root] <= upper.
 */
DTreeInterval dtreeBoundsCircuit(const BooleanCircuit &c, gate_t root,
                                 double max_width, unsigned long budget = 0,
                                 unsigned long *steps_out = nullptr);

} // namespace provsql

#endif
