/**
 * @file BooleanCircuit.h
 * @brief Boolean provenance circuit with support for knowledge compilation.
 *
 * @c BooleanCircuit represents the provenance formula of a query result
 * as a Boolean circuit (AND/OR/NOT/IN gates).  It supports multiple
 * methods for computing the probability of the formula being true under
 * tuple-independent probabilistic databases:
 *
 * | Method                 | Description                                              |
 * |------------------------|----------------------------------------------------------|
 * | @c possibleWorlds()    | Exact enumeration over all 2^n possible worlds           |
 * | @c compilation()       | Knowledge compilation to d-DNNF via an external tool     |
 * | @c monteCarlo()        | Monte Carlo sampling approximation                       |
 * | @c WeightMC()          | Weighted model counting via weightmc                     |
 * | @c independentEvaluation() | Exact evaluation for disconnected circuits           |
 * | @c interpretAsDD()     | Direct tree-decomposition-based compilation              |
 * | @c makeDD()            | Generic d-DNNF construction dispatcher                   |
 *
 * ### Multivalued inputs (@c MULIN / @c MULVAR)
 * Multivalued input gates model tuples drawn from a discrete probability
 * distribution.  @c rewriteMultivaluedGates() rewrites them into standard
 * AND/OR/NOT circuits before knowledge compilation.
 *
 * The circuit is Boost-serialisable for transmission to external processes.
 */
#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <vector>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.hpp"

/**
 * @brief Gate types for a Boolean provenance circuit.
 *
 * - @c UNDETERMINED  Placeholder for a gate whose type has not been set yet.
 * - @c AND           Logical conjunction of child gates.
 * - @c OR            Logical disjunction of child gates.
 * - @c NOT           Logical negation of a single child gate.
 * - @c IN            An input (variable) gate representing a base tuple.
 * - @c MULIN         A multivalued-input gate (one of several options).
 * - @c MULVAR        Auxiliary gate grouping all @c MULIN siblings.
 */
enum class BooleanGate {
  UNDETERMINED, ///< Placeholder gate whose type has not been set yet
  AND,          ///< Logical conjunction of child gates
  OR,           ///< Logical disjunction of child gates
  NOT,          ///< Logical negation of a single child gate
  IN,           ///< Input (variable) gate representing a base tuple
  MULIN,        ///< Multivalued-input gate (one of several options)
  MULVAR        ///< Auxiliary gate grouping all MULIN siblings
};
class dDNNF;

/**
 * @brief Boolean circuit for provenance formula evaluation.
 *
 * Inherits the gate/wire infrastructure from @c Circuit<BooleanGate> and
 * adds probability annotation, info integers (for multivalued inputs), and
 * a rich set of evaluation algorithms.
 */
class BooleanCircuit : public Circuit<BooleanGate> {
private:
/**
 * @brief Evaluate the sub-circuit at @p g on one sampled world.
 *
 * Each gate in @p sampled is treated as @c true; all other @c IN gates
 * are @c false.
 *
 * @param g        Root gate to evaluate.
 * @param sampled  Set of input gates that are @c true in this world.
 * @return         Boolean value of the circuit at gate @p g.
 */
bool evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const;

/**
 * @brief Generate a Tseytin transformation of the sub-circuit at @p g.
 *
 * Produces a CNF/DIMACS-style encoding suitable for input to model
 * counters such as @c weightmc or @c d4.
 *
 * @param g            Root gate.
 * @param display_prob Include probability weights in the output.
 * @return             DIMACS string.
 */
std::string Tseytin(gate_t g, bool display_prob) const;

/**
 * @brief Recursive helper for @c interpretAsDD().
 * @param g     Current gate to process.
 * @param seen  Set of gates already visited (prevents re-processing).
 * @param dd    The d-DNNF being constructed.
 * @return      Gate ID in @p dd corresponding to @p g.
 */
gate_t interpretAsDDInternal(gate_t g, std::set<gate_t> &seen, dDNNF &dd) const;
/**
 * @brief Recursive helper for @c independentEvaluation().
 * @param g     Current gate to evaluate.
 * @param seen  Set of gates already evaluated (for memoisation).
 * @return      Probability at gate @p g.
 */
double independentEvaluationInternal(gate_t g, std::set<gate_t> &seen) const;
/**
 * @brief Recursive helper for @c rewriteMultivaluedGates().
 * @param muls              Gates in the MULVAR group being rewritten.
 * @param cumulated_probs   Cumulative probability thresholds for each MULIN.
 * @param start             First index in @p muls to process.
 * @param end               One past the last index in @p muls to process.
 * @param prefix            Current AND-chain prefix being built.
 */
void rewriteMultivaluedGatesRec(
  const std::vector<gate_t> &muls,
  const std::vector<double> &cumulated_probs,
  unsigned start,
  unsigned end,
  std::vector<gate_t> &prefix);

protected:
std::set<gate_t> inputs;     ///< Set of IN (input) gate IDs
std::set<gate_t> mulinputs;  ///< Set of MULVAR gate IDs
std::vector<double> prob;    ///< Per-gate probability (for IN gates)
std::map<gate_t, unsigned> info; ///< Per-gate integer info (for MULIN gates)
bool probabilistic=false;    ///< @c true if any gate has a non-unit probability

public:
/** @brief Construct an empty Boolean circuit. */
BooleanCircuit() {
}
virtual ~BooleanCircuit() {
}

/** @copydoc Circuit::addGate() */
gate_t addGate() override;
/** @copydoc Circuit::setGate(gateType) */
gate_t setGate(BooleanGate type) override;
/** @copydoc Circuit::setGate(const uuid&, gateType) */
gate_t setGate(const uuid &u, BooleanGate type) override;

/**
 * @brief Create a new gate with a probability annotation.
 * @param t  Gate type (typically @c BooleanGate::IN).
 * @param p  Probability of this gate being @c true.
 * @return   Gate identifier.
 */
gate_t setGate(BooleanGate t, double p);

/**
 * @brief Create (or update) a gate with a UUID and probability.
 * @param u  UUID string.
 * @param t  Gate type.
 * @param p  Probability of this gate being @c true.
 * @return   Gate identifier.
 */
gate_t setGate(const uuid &u, BooleanGate t, double p);

/**
 * @brief Return the set of input (IN) gate IDs.
 * @return Const reference to the set of IN gate identifiers.
 */
const std::set<gate_t> &getInputs() const {
  return inputs;
}

/**
 * @brief Return @c true if the circuit contains any MULIN gates.
 *
 * Multivalued inputs are normally rewritten into AND/OR/NOT/IN gates by
 * @c rewriteMultivaluedGates() before the circuit is consumed by an
 * evaluation method.  Algorithms that cannot handle multivalued inputs
 * directly can use this as a precondition check.
 *
 * @return @c true iff at least one @c MULIN gate is present.
 */
bool hasMultivaluedGates() const {
  return !mulinputs.empty();
}

/**
 * @brief Set the probability for gate @p g and mark the circuit as probabilistic.
 * @param g  Gate identifier.
 * @param p  Probability value in [0, 1].
 */
void setProb(gate_t g, double p) {
  if(!probabilistic && p!=1.)
    probabilistic=true;
  prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p;
}

/**
 * @brief Return the probability stored for gate @p g.
 * @param g  Gate identifier.
 * @return   Probability value.
 */
double getProb(gate_t g) const {
  return prob[static_cast<std::underlying_type<gate_t>::type>(g)];
}

/**
 * @brief Return @c true if any gate has a non-trivial (< 1) probability.
 * @return @c true iff at least one gate has a probability strictly less than 1.
 */
bool isProbabilistic() const {
  return probabilistic;
}

/**
 * @brief Store an integer annotation on gate @p g.
 *
 * Used to record the index of a @c MULIN gate within its @c MULVAR group.
 *
 * @param g     Gate identifier.
 * @param info  Integer to store.
 */
void setInfo(gate_t g, unsigned info);

/**
 * @brief Return the integer annotation for gate @p g.
 * @param g  Gate identifier.
 * @return   Stored integer, or 0 if not set.
 */
unsigned getInfo(gate_t g) const;

/**
 * @brief Compute the probability by exact enumeration of all possible worlds.
 *
 * Only tractable for circuits with a small number of input gates.
 *
 * @param g  Root gate.
 * @return   Exact probability.
 */
double possibleWorlds(gate_t g) const;

/**
 * @brief Compile the sub-circuit rooted at @p g to a @c dDNNF via an external tool.
 *
 * Writes the circuit in DIMACS/DNNF format, invokes @p compiler as a
 * subprocess, and parses the resulting d-DNNF.
 *
 * @param g         Root gate.
 * @param compiler  Command to invoke (e.g. "d4", "c2d", "minic2d").
 * @return          The compiled @c dDNNF.
 */
dDNNF compilation(gate_t g, std::string compiler) const;

/**
 * @brief Estimate the probability via Monte Carlo sampling.
 *
 * @param g        Root gate.
 * @param samples  Number of independent worlds to sample.
 * @return         Estimated probability.
 */
double monteCarlo(gate_t g, unsigned samples) const;

/**
 * @brief Compute the probability using the @c weightmc model counter.
 *
 * @param g    Root gate.
 * @param opt  Additional command-line options forwarded to @c weightmc.
 * @return     Probability estimate returned by @c weightmc.
 */
double WeightMC(gate_t g, std::string opt) const;

/**
 * @brief Compute the probability exactly when inputs are independent.
 *
 * Applicable when the circuit has no shared input gate (i.e., each
 * input appears at most once).
 *
 * @param g  Root gate.
 * @return   Exact probability.
 */
double independentEvaluation(gate_t g) const;

/**
 * @brief Rewrite all MULVAR/MULIN gate clusters into standard AND/OR/NOT circuits.
 *
 * Must be called before any evaluation method when the circuit contains
 * multivalued input gates.
 */
void rewriteMultivaluedGates();

/**
 * @brief Build a @c dDNNF directly from the Boolean circuit's structure.
 *
 * Used as a fallback when no external compiler is available and the
 * circuit is already in a form that can be interpreted as a d-DNNF.
 *
 * @param g  Root gate.
 * @return   A @c dDNNF wrapping the same structure.
 */
dDNNF interpretAsDD(gate_t g) const;

/**
 * @brief Dispatch to the appropriate d-DNNF construction method.
 *
 * @param g       Root gate.
 * @param method  Compilation method name (e.g. "tree-decomposition",
 *                "d4", "c2d", …).
 * @param args    Additional arguments forwarded to the chosen method.
 * @return        The constructed @c dDNNF.
 */
dDNNF makeDD(gate_t g, const std::string &method, const std::string &args) const;

/** @copydoc Circuit::toString() */
virtual std::string toString(gate_t g) const override;

/**
 * @brief Render the sub-circuit at @p g, labelling input gates from a map.
 *
 * Same as @c toString(gate_t), but @c IN and @c MULIN gates whose
 * gate identifier is present in @p labels are rendered using the
 * mapped string instead of the default @c x@<id@> placeholder.  Gates
 * not found in @p labels fall back to the default rendering.
 *
 * @param g       Gate to render.
 * @param labels  Optional mapping from input/mulinput gate IDs to
 *                user-supplied labels.
 * @return        Human-readable string.
 */
std::string toString(
  gate_t g,
  const std::unordered_map<gate_t, std::string> &labels) const;

private:
/**
 * @brief Internal recursive helper for the two @c toString() variants.
 *
 * @param g       Gate to render.
 * @param labels  Pointer to a label map, or @c nullptr for the
 *                unlabelled rendering.
 */
std::string toStringHelper(
  gate_t g,
  const std::unordered_map<gate_t, std::string> *labels) const;

public:

/**
 * @brief Export the circuit in the textual format expected by external compilers.
 *
 * Produces a multi-line string encoding all gates reachable from the
 * circuit in the format used by the standalone @c tdkc tool and external
 * model counters.
 *
 * @param g  Root gate.
 * @return   Circuit description string.
 */
std::string exportCircuit(gate_t g) const;

/**
 * @brief Boost serialisation support.
 * @param ar       Boost archive (input or output).
 * @param version  Archive version (unused).
 */
template<class Archive>
void serialize (Archive & ar, const unsigned int version)
{
  ar & uuid2id;
  ar & id2uuid;
  ar & gates;
  ar & wires;
  ar & inputs;
  ar & mulinputs;
  ar & prob;
  ar & info;
  ar & probabilistic;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;
};

#endif /* BOOLEAN_CIRCUIT_H */
