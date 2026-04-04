/**
 * @file dDNNF.h
 * @brief Decomposable Deterministic Negation Normal Form circuit.
 *
 * A **d-DNNF** (decomposable deterministic Negation Normal Form) is a
 * Boolean circuit with two structural properties:
 * - **Determinism**: for every OR gate, the two sub-formulae are
 *   mutually exclusive (their models are disjoint).
 * - **Decomposability**: for every AND gate, the two sub-formulae share
 *   no variable.
 *
 * These properties enable:
 * - Linear-time computation of the probability of satisfying assignments.
 * - Polynomial computation of Shapley and Banzhaf power indices.
 *
 * @c dDNNF extends @c BooleanCircuit with:
 * - A designated @c root gate.
 * - A memoisation cache for probability evaluation.
 * - Normalisation methods (@c makeSmooth(), @c makeGatesBinary(),
 *   @c simplify()) that transform the circuit into a canonical form
 *   required by the evaluation algorithms.
 * - Conditioning methods (@c condition(), @c conditionAndSimplify())
 *   used during Shapley/Banzhaf computation.
 *
 * ### @c hash_gate_t
 * A standard-library compatible hash functor for @c gate_t, required
 * because @c gate_t is a scoped enum and has no built-in @c std::hash
 * specialisation.
 */
#ifndef DDNNF_H
#define DDNNF_H

#include <string>
#include <unordered_set>

#include "BooleanCircuit.h"

// Forward declaration for friend
class dDNNFTreeDecompositionBuilder;

/**
 * @brief @c std::hash functor for @c gate_t.
 *
 * Allows @c gate_t to be used as a key in @c std::unordered_map and
 * @c std::unordered_set by delegating to @c std::hash on the underlying
 * integer type.
 */
struct hash_gate_t
{
  /**
   * @brief Compute the hash of a gate identifier.
   * @param g  Gate to hash.
   * @return   Hash value suitable for use in unordered containers.
   */
  size_t operator()(gate_t g) const
  {
    return std::hash<typename std::underlying_type<gate_t>::type>()(
      static_cast<typename std::underlying_type<gate_t>::type>(g));
  }
};

/**
 * @brief A d-DNNF circuit supporting exact probabilistic and game-theoretic evaluation.
 *
 * Inherits the full @c BooleanCircuit structure and adds a root gate and
 * algorithms that exploit the d-DNNF structural properties for efficient
 * computation.
 */
class dDNNF : public BooleanCircuit {
private:
/** @brief Memoisation cache mapping gates to their probability values. */
mutable std::unordered_map<gate_t, double, hash_gate_t> probability_cache;

/**
 * @brief Compute the δ table used in the Shapley algorithm.
 *
 * Returns a map from each input gate to a vector of δ values (one per
 * possible "defection count"), as described in the algorithm of
 * Choicenet / Livshits et al.
 * @return Map from each input gate to its vector of δ values.
 */
std::unordered_map<gate_t, std::vector<double> > shapley_delta() const;

/**
 * @brief Compute the α table used in the Shapley algorithm.
 *
 * Returns a 2-D vector of α coefficients that weight the δ values when
 * computing the Shapley value of each input gate.
 * @return 2-D vector of α coefficients (indexed by defection count).
 */
std::vector<std::vector<double> > shapley_alpha() const;

/**
 * @brief Compute the unnormalised Banzhaf value for the whole circuit.
 *
 * Used internally by @c banzhaf() to compute the Banzhaf power index
 * for a specific variable after conditioning.
 *
 * @return Unnormalised Banzhaf value.
 */
double banzhaf_internal() const;

/**
 * @brief Compute a topological ordering of the circuit.
 *
 * @param reversedWires  Adjacency list of the reversed wires.
 * @return               Gates in topological order (leaves first).
 */
std::vector<gate_t> topological_order(const std::vector<std::vector<gate_t> > &reversedWires) const;

gate_t root{0}; ///< The root gate of the d-DNNF

public:
/**
 * @brief Return the root gate of this d-DNNF.
 * @return Gate identifier of the root.
 */
gate_t getRoot() const {
  return root;
}
/**
 * @brief Set the root gate.
 * @param g  New root gate.
 */
void setRoot(gate_t g) {
  root=g;
}

/**
 * @brief Return the set of all variable (IN) gates reachable from @p root.
 * @param root  Root gate of the sub-circuit.
 * @return      Set of reachable IN gate identifiers.
 */
std::unordered_set<gate_t> vars(gate_t root) const;

/**
 * @brief Make the d-DNNF smooth.
 *
 * A d-DNNF is smooth if every OR gate's children mention exactly the
 * same set of variables.  Smoothing is required before probability
 * evaluation to ensure correctness.
 */
void makeSmooth();

/**
 * @brief Rewrite all n-ary AND/OR gates into binary trees.
 *
 * Some evaluation algorithms assume binary gates.  This method replaces
 * every AND (or OR, depending on @p type) gate with more than two
 * children by a balanced binary tree of the same type.
 *
 * @param type  The gate type (@c BooleanGate::AND or @c BooleanGate::OR)
 *              to binarise.
 */
void makeGatesBinary(BooleanGate type);

/**
 * @brief Simplify the d-DNNF by removing redundant constants.
 *
 * Propagates constant @c true / @c false values upward and removes
 * gates that have become trivially constant after conditioning.
 */
void simplify();

/**
 * @brief Condition on variable @p var having value @p value and simplify.
 *
 * Sets input gate @p var to @p value and propagates the simplification
 * through the circuit, returning a new (simplified) @c dDNNF.
 *
 * @param var    Input gate to condition on.
 * @param value  Truth value to assign to @p var.
 * @return       A simplified copy of the conditioned circuit.
 */
dDNNF conditionAndSimplify(gate_t var, bool value) const;

/**
 * @brief Condition on variable @p var having value @p value (no simplification).
 *
 * @param var    Input gate to condition on.
 * @param value  Truth value to assign to @p var.
 * @return       A copy of the circuit with @p var fixed to @p value.
 */
dDNNF condition(gate_t var, bool value) const;

/**
 * @brief Compute the exact probability of the d-DNNF being @c true.
 *
 * Requires the circuit to be smooth.  Uses the structural properties of
 * the d-DNNF to evaluate in time linear in the circuit size.
 *
 * @return Probability in [0, 1].
 */
double probabilityEvaluation() const;

/**
 * @brief Compute the Shapley value of input gate @p var.
 *
 * Implements the polynomial-time Shapley-value algorithm for d-DNNFs
 * described in Livshits et al. (PODS 2021).
 *
 * @param var  Input gate whose Shapley value is requested.
 * @return     Shapley value.
 */
double shapley(gate_t var) const;

/**
 * @brief Compute the Banzhaf power index of input gate @p var.
 *
 * Uses repeated conditioning and probability evaluation.
 *
 * @param var  Input gate whose Banzhaf index is requested.
 * @return     Banzhaf power index.
 */
double banzhaf(gate_t var) const;

friend dDNNFTreeDecompositionBuilder; ///< Allowed to construct and populate this d-DNNF
friend dDNNF BooleanCircuit::compilation(gate_t g, std::string compiler) const; ///< Allowed to access internal d-DNNF state
};

#endif /* DDNNF_H */
