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
                          double max_width);

} // namespace provsql

#endif
