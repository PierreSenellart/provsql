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
 * @brief Replace a @c gate_cmp by a constant Boolean leaf
 *        (@c gate_one for @p p == 1, @c gate_zero for @p p == 0)
 *        or by a Bernoulli @c gate_input for any other @p p.
 *
 * Used by peephole pruning passes when a comparator's probability is
 * provably 0, 1, or a closed-form value.  Distinguishing the
 * 0 / 1 case from the fractional case matters because non-probabilistic
 * semirings (e.g. @c sr_formula, @c sr_counting) have well-defined
 * @c zero() / @c one() values but no notion of "Bernoulli with
 * probability @c p" &ndash; an unknown @c gate_input would default
 * to @c semiring.one() in every semiring (per
 * @c GenericCircuit::evaluate), which is wrong for an
 * always-false comparator.  Using @c gate_zero / @c gate_one
 * directly is universally correct: every semiring knows its
 * identities.
 *
 * Fractional probabilities are still encoded as @c gate_input + a
 * probability so probability evaluators (MC, independent, treedec,
 * d-DNNF, d4) can consume them, but only those evaluators handle
 * non-trivial probabilities meaningfully.  Such resolutions should
 * therefore be confined to passes invoked from a probability
 * context (not the universal @c getGenericCircuit-time pass).
 *
 * Operates on the in-memory circuit only; the persistent mmap store
 * is never mutated.  Children that become orphaned are not reaped
 * here.
 */
void resolveCmpToBernoulli(gate_t g, double p) {
  if (p == 0.0) {
    setGateType(g, gate_zero);
  } else if (p == 1.0) {
    setGateType(g, gate_one);
  } else {
    setGateType(g, gate_input);
    setProb(g, p);
    inputs.insert(g);
  }
  getWires(g).clear();
  infos.erase(g);
  extra.erase(g);
}

/**
 * @brief Replace an arbitrary gate (typically @c gate_times) by
 *        @c gate_zero.
 *
 * Used by RangeCheck's joint-conjunction pass when an AND of cmps
 * over a shared RV constrains its support to an empty interval:
 * since @c gate_zero is the multiplicative absorber in every
 * semiring, replacing a @c gate_times with it is universally sound,
 * and orphans the conjuncts (their effects are now unreachable
 * from the root, regardless of what each individual cmp would
 * resolve to).
 *
 * The wires, infos and extra fields are cleared so the gate is a
 * proper leaf.  Operates on the in-memory circuit only.
 */
void resolveGateToZero(gate_t g) {
  setGateType(g, gate_zero);
  getWires(g).clear();
  infos.erase(g);
  extra.erase(g);
}

/**
 * @brief Rewrite an arbitrary gate as a @c gate_value carrying the
 *        textual extra @p s.
 *
 * Used by the @c HybridEvaluator simplifier when a @c gate_arith
 * subtree constant-folds to a scalar.  Same wire/info/extra clearing
 * as @c resolveCmpToBernoulli &ndash; the old children become
 * orphans relative to @p g.  @p s is interpreted by the consumer
 * via @c parseDoubleStrict (or analogous routines), so it must be
 * a canonical textual representation that round-trips through
 * @c std::stod.  Operates on the in-memory circuit only.
 */
void resolveToValue(gate_t g, const std::string &s) {
  setGateType(g, gate_value);
  getWires(g).clear();
  infos.erase(g);
  extra[g] = s;
}

/**
 * @brief Rewrite an arbitrary gate as a @c gate_rv carrying the
 *        distribution-spec extra @p s.
 *
 * Used by the @c HybridEvaluator simplifier when a linear
 * combination of independent normals (or i.i.d. exponentials with
 * the same rate) collapses to a single closed-form distribution.
 * @p s must be a textual encoding parseable by
 * @c parse_distribution_spec.  Same wire/info clearing as
 * @c resolveCmpToBernoulli.  Operates on the in-memory circuit only.
 */
void resolveToRv(gate_t g, const std::string &s) {
  setGateType(g, gate_rv);
  getWires(g).clear();
  infos.erase(g);
  extra[g] = s;
}

/**
 * @brief Drop semiring identity wires and collapse single-wire
 *        @c gate_times / @c gate_plus to their lone non-identity
 *        child; collapse a @c gate_times containing a @c gate_zero
 *        wire to that absorber.
 *
 * Universal rewrite: the multiplicative identity (@c gate_one), the
 * additive identity (@c gate_zero), and the multiplicative absorber
 * (@c gate_zero) hold across every provsql semiring, so a single
 * pass after @c RangeCheck is sound for every downstream consumer
 * (probability_evaluate, to_provxml, view_circuit, Studio's
 * simplified subgraph).  Does NOT apply the additive absorber
 * rewrite (@c plus-with-one): @c gate_one only absorbs in
 * idempotent semirings (Boolean, MinMax), so applying it
 * unconditionally would silently change the semantics for
 * @c Counting / @c Formula / etc.
 *
 * Operates on the in-memory circuit only; the persistent mmap store
 * is never touched.  Gated alongside @c RangeCheck by
 * @c provsql.simplify_on_load.
 */
void foldSemiringIdentities();

/**
 * @brief Replace the wires of @p g with @p w.
 *
 * Used by the @c HybridEvaluator simplifier's identity-element drop
 * to remove constant-zero wires from a @c PLUS gate (or constant-one
 * wires from a @c TIMES gate) without changing the gate's type.
 * Dropped children become orphans relative to @p g.
 */
void setWires(gate_t g, std::vector<gate_t> w) {
  getWires(g) = std::move(w);
}

/**
 * @brief Rewrite an arbitrary gate as a @c gate_plus over @p w.
 *
 * Used by the @c HybridEvaluator multi-cmp island decomposer when
 * a comparator from a shared-island group is rewritten as the OR
 * of the joint-table @c gate_mulinput leaves where the comparator's
 * bit is set.  Clears infos and extra and installs the new wires.
 */
void resolveToPlus(gate_t g, std::vector<gate_t> w) {
  setGateType(g, gate_plus);
  getWires(g) = std::move(w);
  infos.erase(g);
  extra.erase(g);
}

/**
 * @brief Allocate a fresh @c gate_input gate carrying probability
 *        @p p, with a unique synthetic UUID so subsequent
 *        @c BooleanCircuit conversion does not collide multiple
 *        no-UUID inputs onto the same gate.
 *
 * The synthetic UUID is derived from the freshly-assigned gate id;
 * it is not a real v4 UUID (does not match the @c xxxxxxxx-...
 * format) and exists only for in-memory uniqueness during the
 * probability_evaluate pipeline.  The gate is added to @c inputs
 * so the conversion's first loop maps it into @c BooleanCircuit's
 * @c gc_to_bc.
 *
 * @param p  Probability for the new input.
 * @return   The id of the new gate.
 */
gate_t addAnonymousInputGate(double p) {
  gate_t id = addGate();
  setGateType(id, gate_input);
  setProb(id, p);
  std::string u = "dec-in-" + std::to_string(static_cast<size_t>(id));
  uuid2id[u] = id;
  id2uuid[id] = u;
  inputs.insert(id);
  return id;
}

/**
 * @brief Allocate a fresh @c gate_mulinput gate with key @p key,
 *        probability @p p, and value index @p value_index.
 *
 * Used by the joint-table decomposer to materialise one Bernoulli
 * outcome of a 2^k-way categorical distribution over a shared
 * continuous island.  All mulinputs in one joint table share the
 * same @p key (the block anchor returned by @c addAnonymousInputGate);
 * @p value_index is stored in @c info1 so
 * @c BooleanCircuit::independentEvaluation can group / dedup
 * MULIN references at OR sites.  A unique synthetic UUID is
 * minted for the same reason as @c addAnonymousInputGate.
 */
gate_t addAnonymousMulinputGate(gate_t key, double p,
                                unsigned value_index) {
  gate_t id = addGate();
  setGateType(id, gate_mulinput);
  setProb(id, p);
  setInfos(id, value_index, 0);
  getWires(id).push_back(key);
  std::string u = "dec-mul-" + std::to_string(static_cast<size_t>(id));
  uuid2id[u] = id;
  id2uuid[id] = u;
  return id;
}

/**
 * @brief Allocate a fresh @c gate_arith gate with operator tag @p op
 *        and the given @p wires.
 *
 * Used by the @c HybridEvaluator simplifier's mixture-lift rule when
 * pushing a @c PLUS / @c TIMES inside a mixture's two scalar branches:
 * each branch needs a fresh @c gate_arith child carrying the lifted
 * operation, and those children must round-trip through downstream
 * @c id2uuid / @c uuid2id lookups (Studio's simplified subgraph,
 * @c to_provxml).  A unique synthetic UUID is minted for the same
 * reason as @c addAnonymousInputGate.
 */
gate_t addAnonymousArithGate(provsql_arith_op op,
                             std::vector<gate_t> wires_) {
  gate_t id = addGate();
  setGateType(id, gate_arith);
  setInfos(id, static_cast<int>(op), 0);
  getWires(id) = std::move(wires_);
  std::string u = "dec-arith-" + std::to_string(static_cast<size_t>(id));
  uuid2id[u] = id;
  id2uuid[id] = u;
  return id;
}

/**
 * @brief Rewrite @p g in place as a @c gate_mixture over the wires
 *        @c [p_token, x_token, y_token].
 *
 * Used by the @c HybridEvaluator simplifier's mixture-lift rule when
 * a @c gate_arith containing a single @c gate_mixture child is pushed
 * inside the mixture's branches: the outer arith gate is rewritten in
 * place as the lifted mixture, preserving its UUID and the
 * non-mixture-aware code paths that already hold references to it.
 * The @p p_token is reused verbatim so Bernoulli identity is
 * preserved across the rewrite; the @p x_token and @p y_token are
 * the freshly-minted arith children built via
 * @c addAnonymousArithGate.
 */
void resolveToMixture(gate_t g, gate_t p_token,
                      gate_t x_token, gate_t y_token) {
  setGateType(g, gate_mixture);
  std::vector<gate_t> w;
  w.reserve(3);
  w.push_back(p_token);
  w.push_back(x_token);
  w.push_back(y_token);
  getWires(g) = std::move(w);
  infos.erase(g);
  extra.erase(g);
}

/**
 * @brief Allocate a fresh @c gate_mulinput labelled with a numeric
 *        outcome value carried in @c extra.
 *
 * Variant of @c addAnonymousMulinputGate used by the categorical
 * mixture lowering: the mulinput's @c info1 still holds the ordinal
 * @p value_index for @c independentEvaluation's dedup, and the
 * outcome's numeric label is stored in @c extra so the evaluator-side
 * categorical-mixture handlers can read it as a @c float8.
 */
gate_t addAnonymousMulinputGateWithValue(gate_t key, double p,
                                         unsigned value_index,
                                         const std::string &value_text) {
  gate_t id = addAnonymousMulinputGate(key, p, value_index);
  setExtra(id, value_text);
  return id;
}

/**
 * @brief Rewrite @p g in place as a categorical-form @c gate_mixture
 *        over @p wires (@c [key, mul_1, ..., mul_n]).
 *
 * Used by the Dirac-mixture-collapse simplifier: when a chained
 * @c gate_mixture tree's leaves are all @c gate_value, the structure
 * is the same as a categorical RV.  We reuse the @c gate_mixture
 * type with @c N > 3 wires, where @c wires[0] is a fresh
 * @c gate_input "key" anchor (its own probability is irrelevant: the
 * categorical mass is on the mulinputs) and @c wires[1..n] are
 * @c gate_mulinput leaves sharing that key.  Every @c gate_mixture
 * handler downstream of the simplifier branches on
 * <tt>wires.size() == 3</tt> for the classic
 * <tt>[p_token, x_token, y_token]</tt> shape vs the categorical
 * shape; the latter is what unlocks closed-form CDF / cmp evaluation
 * via @c AnalyticEvaluator.
 */
void resolveToCategoricalMixture(gate_t g, std::vector<gate_t> wires_) {
  setGateType(g, gate_mixture);
  getWires(g) = std::move(wires_);
  infos.erase(g);
  extra.erase(g);
}

/**
 * @brief Test whether @p g is a categorical-form @c gate_mixture
 *        (the Dirac-mixture collapse output).
 *
 * Returns true iff @p g is a @c gate_mixture whose wires are
 * @c [key, mul_1, ..., mul_n] with @p n &ge; 1: @c wires[0] a
 * @c gate_input key anchor, and every subsequent wire a
 * @c gate_mulinput.  The classic mixture shape
 * @c [p_token, x_token, y_token] returns false (one or both of
 * @c wires[1..2] are not @c gate_mulinput).
 */
bool isCategoricalMixture(gate_t g) const
{
  if (getGateType(g) != gate_mixture) return false;
  const auto &w = getWires(g);
  if (w.size() < 2) return false;
  if (getGateType(w[0]) != gate_input) return false;
  for (std::size_t i = 1; i < w.size(); ++i) {
    if (getGateType(w[i]) != gate_mulinput) return false;
  }
  return true;
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
