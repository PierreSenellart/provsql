/**
 * @file dDNNFTreeDecompositionBuilder.h
 * @brief Constructs a d-DNNF from a Boolean circuit and its tree decomposition.
 *
 * @c dDNNFTreeDecompositionBuilder implements the knowledge-compilation
 * algorithm described in:
 * > A. Amarilli, F. Capelli, M. Monet, P. Senellart,
 * >  "Connecting Knowledge Compilation Classes and Width Parameters".
 * >  Theory of Computing Systems 64(5):861–914, 2020.
 * >  https://doi.org/10.1007/s00224-019-09930-2
 *
 * The algorithm traverses the tree decomposition bottom-up.  At each
 * bag it enumerates *valuations* (assignments of Boolean values to the
 * gates in the bag) that are consistent with the circuit's constraints.
 * For each valuation it builds a d-DNNF AND gate whose children are the
 * (negated) input gates for that valuation.  The OR combination of all
 * valid valuations at the root bag gives the final d-DNNF.
 *
 * ### Key types
 * - @c valuation_t: a `flat_map<gate_t, bool>` (bounded by treewidth+1)
 *   mapping each in-bag gate to its truth value.
 * - @c suspicious_t: a `flat_set<gate_t>` of gates whose values have not
 *   yet been "confirmed" by their responsible bag.
 * - @c dDNNFGate: an intermediate gate in the constructed d-DNNF, bundling
 *   a gate ID with a valuation and a suspicious set.
 * - @c gates_to_or_t: the main DP table mapping (valuation, suspicious)
 *   pairs to lists of @c dDNNFGate children to be OR'd.
 *
 * ### Usage
 * ```cpp
 * dDNNFTreeDecompositionBuilder builder(circuit, root_gate, td);
 * dDNNF dd = std::move(builder).build();
 * ```
 */
#ifndef dDNNF_TREE_DECOMPOSITION_BUILDER_H
#define dDNNF_TREE_DECOMPOSITION_BUILDER_H

#include <cassert>
#include <unordered_map>
#include <map>

#include <boost/container/static_vector.hpp>

#include "flat_map.hpp"
#include "flat_set.hpp"
#include "TreeDecomposition.h"
#include "dDNNF.h"
#include "BooleanCircuit.h"

/**
 * @brief Builds a d-DNNF from a Boolean circuit using a tree decomposition.
 *
 * This class is a one-shot builder: construct it, call @c build(), and
 * the object is consumed (move-only).
 */
class dDNNFTreeDecompositionBuilder
{
public:
/** @brief Stack-allocated vector bounded by the maximum treewidth + 1. */
template<class T>
using small_vector = boost::container::static_vector<T, TreeDecomposition::MAX_TREEWIDTH+1>;

/** @brief Partial assignment of truth values to the gates of a bag. */
using valuation_t = flat_map<gate_t, bool, small_vector>;

/** @brief Set of gates whose truth-value assignments are not yet confirmed. */
using suspicious_t = flat_set<gate_t, small_vector>;

/** @brief Generic bounded-capacity vector for intermediate d-DNNF gates. */
template<class T>
using gate_vector_t = std::vector<T>;

/** @brief Dynamic-programming table: (valuation, suspicious) → list of children. */
using gates_to_or_t =
  std::unordered_map<valuation_t, std::map<suspicious_t, gate_vector_t<gate_t> > >;

private:
const BooleanCircuit &c;   ///< Source circuit
gate_t root_id;             ///< Root gate of the source circuit
TreeDecomposition &td;      ///< Tree decomposition of the circuit's primal graph
dDNNF d;                    ///< The d-DNNF being constructed
std::unordered_map<gate_t, bag_t> responsible_bag; ///< Maps each gate to its "responsible" bag
std::unordered_map<gate_t, gate_t> input_gate;     ///< Maps original IN gates to d-DNNF IN gates
std::unordered_map<gate_t, gate_t> negated_input_gate; ///< Maps original IN gates to their negations
std::set<std::pair<gate_t, gate_t> > wiresSet;     ///< Set of all wires in the source circuit

/**
 * @brief Intermediate representation of a partially built d-DNNF gate.
 *
 * Each @c dDNNFGate carries a d-DNNF gate ID together with the current
 * bag's valuation and the set of suspicious gates, allowing the DP
 * algorithm to combine compatible gates across bags.
 */
struct dDNNFGate {
  gate_t id;              ///< Gate ID in the target d-DNNF
  valuation_t valuation;  ///< Current bag's truth-value assignment
  suspicious_t suspicious;///< Gates whose assignments are unconfirmed

  /**
   * @brief Construct a @c dDNNFGate.
   * @param i  Gate ID in the target d-DNNF.
   * @param v  Current bag's truth-value assignment.
   * @param s  Set of suspicious (unconfirmed) gates.
   */
  dDNNFGate(gate_t i, valuation_t v, suspicious_t s) :
    id{i}, valuation{std::move(v)}, suspicious{std::move(s)} {
  }
};

/**
 * @brief Group a list of @c dDNNFGate entries by (valuation, suspicious) pairs.
 *
 * Used when processing a bag's children to identify which sub-results
 * can be OR'd together.
 *
 * @param bag      Current bag being processed.
 * @param gates    List of d-DNNF gates from child bags.
 * @param partial  Partially accumulated DP table from previous children.
 * @return         Updated DP table.
 */
[[nodiscard]] gates_to_or_t collectGatesToOr(
  bag_t bag,
  const gate_vector_t<dDNNFGate> &gates,
  const gates_to_or_t &partial);

/**
 * @brief Build the d-DNNF contributions for a leaf bag.
 *
 * Enumerates all consistent valuations for the gates in the leaf bag
 * and creates the corresponding d-DNNF AND gates.
 *
 * @param bag  The leaf bag to process.
 * @return     List of @c dDNNFGate entries, one per consistent valuation.
 */
[[nodiscard]] gate_vector_t<dDNNFGate> builddDNNFLeaf(bag_t bag);

/**
 * @brief Main recursive procedure: build the d-DNNF bottom-up.
 *
 * @return List of @c dDNNFGate entries at the root bag.
 */
[[nodiscard]] gate_vector_t<dDNNFGate> builddDNNF();

/**
 * @brief Return @c true if there is a wire from gate @p u to gate @p v.
 * @param u  Source gate.
 * @param v  Target gate.
 * @return   @c true if a wire from @p u to @p v exists in the source circuit.
 */
[[nodiscard]] bool circuitHasWire(gate_t u, gate_t v) const;

/**
 * @brief Return @c true if @p valuation is a "almost valuation".
 *
 * A valuation is "almost" if every gate in it that must have its value
 * confirmed by the current bag has indeed been assigned a value.
 *
 * @param valuation  The valuation to test.
 * @return           @c true if the valuation is consistent with circuit constraints.
 */
[[nodiscard]] bool isAlmostValuation(
  const valuation_t &valuation) const;

/**
 * @brief Compute the subset of @p innocent that remains innocent.
 *
 * Returns the gates in @p innocent whose suspicious status is not
 * overridden by @p valuation.
 *
 * @param valuation  Current bag's valuation.
 * @param innocent   Candidate innocent set.
 * @return           The actually innocent gates.
 */
[[nodiscard]] suspicious_t getInnocent(
  const valuation_t &valuation,
  const suspicious_t &innocent) const;

public:
/**
 * @brief Construct the builder for a specific circuit and tree decomposition.
 *
 * Pre-computes the wire set for fast @c circuitHasWire() queries.
 *
 * @param circuit            Source Boolean circuit.
 * @param gate               Root gate of @p circuit to compile.
 * @param tree_decomposition Tree decomposition of @p circuit's primal graph.
 */
dDNNFTreeDecompositionBuilder(
  const BooleanCircuit &circuit,
  gate_t gate,
  TreeDecomposition &tree_decomposition) : c{circuit}, td{tree_decomposition}
{
  root_id = gate;

  for(gate_t i{0}; i<c.getNbGates(); ++i)
    for(auto g: c.getWires(i))
      wiresSet.emplace(i,g);
};

/**
 * @brief Execute the compilation and return the resulting d-DNNF.
 *
 * This is a move-only operation: the builder object is consumed.
 *
 * @return The compiled @c dDNNF (as an rvalue).
 */
[[nodiscard]] dDNNF&& build() &&;

friend std::ostream &operator<<(std::ostream &o, const dDNNFGate &g);
};

/**
 * @brief Debug output for a @c dDNNFTreeDecompositionBuilder::dDNNFGate.
 * @param o  Output stream.
 * @param g  Gate to display.
 * @return   Reference to @p o.
 */
std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::dDNNFGate &g);

#endif /* dDNNF_TREE_DECOMPOSITION_BUILDER_H  */
