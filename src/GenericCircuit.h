/**
 * @file GenericCircuit.h
 * @brief Semiring-agnostic in-memory provenance circuit.
 *
 * @c GenericCircuit is an in-memory directed acyclic graph whose gate
 * types use the PostgreSQL @c gate_type enumeration.  It is built from
 * the persistent mmap representation (via @c createGenericCircuit() /
 * @c getGenericCircuit()) and then evaluated over an arbitrary semiring
 * using the @c evaluate() template method.
 *
 * Beyond the gate/wire data inherited from @c Circuit<gate_type>,
 * @c GenericCircuit tracks:
 * - Per-gate integer annotation pairs (@c info1, @c info2) used by
 *   aggregation and semimodule gates.
 * - Per-gate variable-length string extras (e.g. label strings for
 *   @c gate_value gates).
 * - A probability vector for probabilistic evaluation.
 * - The set of input gate IDs (for semiring evaluation traversal).
 *
 * The circuit is Boost-serialisable, which is used when sending it as
 * a blob to an external knowledge-compiler process.
 */
#ifndef GENERIC_CIRCUIT_H
#define GENERIC_CIRCUIT_H

#include <map>
#include <type_traits>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.h"
#include "semiring/Semiring.h"

extern "C" {
#include "provsql_utils.h"
}

/**
 * @brief In-memory provenance circuit with semiring-generic evaluation.
 *
 * Gate types are the PostgreSQL @c gate_type values.  The circuit is
 * constructed from the persistent mmap store and then evaluated in-memory.
 */
class GenericCircuit : public Circuit<gate_type>
{
private:
std::map<gate_t, std::pair<unsigned,unsigned> > infos; ///< Per-gate (info1, info2) annotations
std::map<gate_t, std::string> extra;                   ///< Per-gate string extras
std::set<gate_t> inputs;                               ///< Set of input (leaf) gate IDs
std::vector<double> prob;                              ///< Per-gate probability values

public:
/**
 * @brief Return a placeholder debug string (not intended for display).
 * @param g  Gate identifier (unused).
 * @return   Fixed placeholder string @c "<GenericCircuit>".
 */
virtual std::string toString(gate_t g) const override {
  return "<GenericCircuit>";
}

/**
 * @brief Set the integer annotation pair for gate @p g.
 * @param g     Gate identifier.
 * @param info1 First annotation integer.
 * @param info2 Second annotation integer.
 */
void setInfos(gate_t g, unsigned info1, unsigned info2)
{
  infos[g]=std::make_pair(info1, info2);
}

/**
 * @brief Return the integer annotation pair for gate @p g.
 * @param g  Gate identifier.
 * @return   @c {info1, info2}, or @c {-1,-1} if not set.
 */
std::pair<unsigned,unsigned> getInfos(gate_t g) const
{
  auto it = infos.find(g);
  if(it==infos.end())
    return std::make_pair(-1, -1);
  return it->second;
}

/**
 * @brief Attach a string extra to gate @p g.
 * @param g   Gate identifier.
 * @param ex  String to store.
 */
void setExtra(gate_t g, const std::string &ex)
{
  extra[g]=ex;
}

/**
 * @brief Return the string extra for gate @p g.
 * @param g  Gate identifier.
 * @return   The stored string, or empty if not set.
 */
std::string getExtra(gate_t g) const
{
  auto it = extra.find(g);
  if(it==extra.end())
    return "";
  else
    return it->second;
}

/** @copydoc Circuit::addGate() */
gate_t addGate() override;
/** @copydoc Circuit::setGate(gateType) */
gate_t setGate(gate_type type) override;
/** @copydoc Circuit::setGate(const uuid&, gateType) */
gate_t setGate(const uuid &u, gate_type type) override;

/**
 * @brief Return the set of input (leaf) gates.
 * @return Const reference to the set of input gate identifiers.
 */
const std::set<gate_t> &getInputs() const {
  return inputs;
}

/**
 * @brief Set the probability for gate @p g.
 * @param g  Gate identifier.
 * @param p  Probability in [0, 1].
 */
void setProb(gate_t g, double p) {
  prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p;
}

/**
 * @brief Return the probability for gate @p g.
 * @param g  Gate identifier.
 * @return   The stored probability.
 */
double getProb(gate_t g) const {
  return prob[static_cast<std::underlying_type<gate_t>::type>(g)];
}

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
  ar & infos;
  ar & extra;
  ar & inputs;
  ar & prob;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;

/**
 * @brief Evaluate the sub-circuit rooted at gate @p g over semiring @p semiring.
 *
 * Performs a post-order traversal from @p g, mapping each input gate to
 * its semiring value via @p provenance_mapping, and combining the results
 * using the semiring operations.
 *
 * @tparam S                  A concrete @c semiring::Semiring subclass.
 * @param g                   Root gate of the sub-circuit to evaluate.
 * @param provenance_mapping  Map from input gate IDs to semiring values.
 * @param semiring            Semiring instance providing @c zero(), @c one(),
 *                            @c plus(), @c times(), etc.
 * @return                    The semiring value of the circuit at gate @p g.
 */
template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> = 0>
typename S::value_type evaluate(gate_t g, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping, S semiring) const;
};

#endif /* GENERIC_CIRCUIT_H */
