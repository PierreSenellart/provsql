/**
 * @file DotCircuit.h
 * @brief Provenance circuit variant that renders to GraphViz DOT format.
 *
 * @c DotCircuit is a lightweight circuit specialisation used exclusively
 * for visualisation.  It stores a textual description (@c desc) alongside
 * each gate so that the rendered DOT graph can label nodes with meaningful
 * provenance formula symbols (⊗, ⊕, ⊖, δ, etc.).
 *
 * The @c render() method traverses the circuit and produces a complete
 * GraphViz @c digraph string that can be piped to @c dot or displayed
 * with @c graph-easy.
 */
#ifndef DOT_CIRCUIT_H
#define DOT_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "Circuit.hpp"

/**
 * @brief Gate types for a DOT visualisation circuit.
 *
 * Each value corresponds to a semiring operator or special gate kind,
 * and is rendered as the appropriate mathematical symbol in the output.
 *
 * - @c UNDETERMINED  Placeholder, should not appear in a finished circuit.
 * - @c OTIMES        Semiring times (⊗).
 * - @c OPLUS         Semiring plus (⊕).
 * - @c OMINUS        Semiring monus (⊖), full.
 * - @c OMINUSR       Monus, right child only.
 * - @c OMINUSL       Monus, left child only.
 * - @c PROJECT       Projection gate.
 * - @c EQ            Equijoin gate.
 * - @c IN            Input (variable) gate.
 * - @c DELTA         δ-semiring operator.
 */
enum class DotGate
{
  UNDETERMINED, ///< Placeholder gate not yet assigned a type
  OTIMES,       ///< Semiring times (⊗)
  OPLUS,        ///< Semiring plus (⊕)
  OMINUS,       ///< Semiring monus (⊖), full
  OMINUSR,      ///< Monus, right child only
  OMINUSL,      ///< Monus, left child only
  PROJECT,      ///< Projection gate
  EQ,           ///< Equijoin gate
  IN,           ///< Input (variable) gate
  DELTA         ///< δ-semiring operator
};

/**
 * @brief Circuit specialisation for GraphViz DOT rendering.
 *
 * Extends @c Circuit<DotGate> with per-gate description strings and a
 * @c render() method that produces a complete DOT @c digraph.
 */
class DotCircuit : public Circuit<DotGate> {
private:
std::set<gate_t> inputs;    ///< Input gate IDs (rendered as leaf nodes)
std::vector<std::string> desc; ///< Per-gate label strings (indexed by gate ID)

public:
/** @copydoc Circuit::addGate() */
gate_t addGate() override;
using Circuit::setGate;

/** @copydoc Circuit::setGate(const uuid&, gateType) */
gate_t setGate(const uuid &u, DotGate type) override;

/**
 * @brief Create (or update) a gate with a UUID, type, and description.
 *
 * The @p d string is used as the node label in the DOT output.
 *
 * @param u  UUID of the gate.
 * @param t  Gate type.
 * @param d  Description / label string for this gate.
 * @return   Gate identifier.
 */
gate_t setGate(const uuid &u, DotGate t, std::string d);

/**
 * @brief Render the entire circuit as a GraphViz DOT @c digraph string.
 *
 * The returned string can be written to a file and processed with
 * @c dot, @c neato, or @c graph-easy.
 *
 * @return DOT representation of the circuit.
 */
std::string render() const;

/** @copydoc Circuit::toString() */
virtual std::string toString(gate_t g) const override;
};

#endif /* DOT_CIRCUIT_H */
