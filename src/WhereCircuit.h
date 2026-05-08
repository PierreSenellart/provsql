/**
 * @file WhereCircuit.h
 * @brief Where-provenance circuit tracking column-level data origin.
 *
 * Where-provenance records not just which base tuples contributed to a
 * query result, but also *which attribute values* in those base tuples
 * gave rise to each output column.
 *
 * @c WhereCircuit extends @c Circuit<WhereGate> with per-gate metadata
 * that describes:
 * - **Input gates** – the source table, row UUID, and number of columns.
 * - **Projection gates** – the list of attribute positions being projected.
 * - **Equality gates** – the pair of attribute positions being joined.
 *
 * The @c evaluate() method traverses the circuit and returns, for each
 * output position, a set of @c Locator values identifying the base
 * (table, tuple, column) triples that the value was copied from.
 */
#ifndef WHERE_CIRCUIT_H
#define WHERE_CIRCUIT_H

#include <set>
#include <vector>
#include <utility>

#include "Circuit.hpp"

/**
 * @brief Gate types for a where-provenance circuit.
 *
 * - @c UNDETERMINED  Placeholder; should not appear in a complete circuit.
 * - @c TIMES         Product (conjunction) of child where-provenance sets.
 * - @c PLUS          Sum (disjunction) of child where-provenance sets.
 * - @c EQ            Equijoin gate recording the joined attribute pair.
 * - @c PROJECT       Projection gate recording which attributes are kept.
 * - @c IN            Input gate for a single base-relation tuple.
 */
enum class WhereGate {
  UNDETERMINED, ///< Placeholder; should not appear in a complete circuit
  TIMES,        ///< Product (conjunction) of child where-provenance sets
  PLUS,         ///< Sum (disjunction) of child where-provenance sets
  EQ,           ///< Equijoin gate recording the joined attribute pair
  PROJECT,      ///< Projection gate recording which attributes are kept
  IN            ///< Input gate for a single base-relation tuple
};

/**
 * @brief Circuit encoding where-provenance (column-level data origin).
 *
 * Stores extra per-gate metadata that relates each gate to its position
 * in the original query plan (table name, row UUID, column index).
 */
class WhereCircuit : public Circuit<WhereGate> {
 private:
  std::unordered_map<gate_t, uuid> input_token;                      ///< UUID of the source tuple for each IN gate
  std::unordered_map<gate_t, std::pair<std::string,int>> input_info; ///< (table name, nb_columns) for each IN gate
  std::unordered_map<gate_t, std::vector<int>> projection_info;      ///< Projected attribute positions for PROJECT gates
  std::unordered_map<gate_t, std::pair<int,int>> equality_info;      ///< Joined attribute pair (pos1, pos2) for EQ gates

 public:
  /** @copydoc Circuit::setGate(const uuid&, gateType) */
  gate_t setGate(const uuid &u, WhereGate type) override;

  /**
   * @brief Create an input gate for a specific table row.
   *
   * @param u           UUID of the source tuple (row token).
   * @param table       Name of the source relation.
   * @param nb_columns  Number of columns in the source tuple.
   * @return            Gate identifier.
   */
  gate_t setGateInput(const uuid &u, std::string table, int nb_columns);

  /**
   * @brief Create a projection gate with column mapping.
   *
   * @param u      UUID to associate with this gate.
   * @param infos  List of attribute positions surviving the projection.
   * @return       Gate identifier.
   */
  gate_t setGateProjection(const uuid &u, std::vector<int> &&infos);

  /**
   * @brief Create an equality (equijoin) gate for two attribute positions.
   *
   * @param u     UUID to associate with this gate.
   * @param pos1  Left-side attribute position.
   * @param pos2  Right-side attribute position.
   * @return      Gate identifier.
   */
  gate_t setGateEquality(const uuid &u, int pos1, int pos2);

  /** @copydoc Circuit::toString() */
  std::string toString(gate_t g) const override;

 private:
  /**
   * @brief Recursive helper for @c toString that propagates the parent
   * gate type for parenthesis elision.
   *
   * The @p parent parameter drives the wrap decision: at the root
   * (parent set to @c UNDETERMINED) the outer parens are dropped, and
   * when @p parent matches the current gate type (associative
   * TIMES/PLUS) the wrap is dropped to flatten same-op nesting. A
   * 1-wire TIMES/PLUS also bypasses the wrap and delegates to its
   * single child.
   */
  std::string toStringHelper(gate_t g, WhereGate parent) const;
 public:

  /**
   * @brief Describes the origin of a single attribute value.
   *
   * A @c Locator identifies a specific attribute of a specific tuple in a
   * specific base table as the origin of an output value.
   */
  struct Locator {
    std::string table; ///< Name of the source relation
    uuid tid;          ///< UUID (row token) of the source tuple
    int position;      ///< Zero-based column index within the tuple

    /**
     * @brief Construct a @c Locator.
     * @param t  Source table name.
     * @param u  Source tuple UUID.
     * @param i  Column position.
     */
    Locator(std::string t, uuid u, int i) : table(t), tid(u), position(i) {}
    /**
     * @brief Lexicographic ordering for use in @c std::set.
     * @param that  Other @c Locator.
     * @return @c true if @c *this is less than @p that.
     */
    bool operator<(Locator that) const;
    /**
     * @brief Return a human-readable representation of this locator.
     * @return String of the form "table[tid].position".
     */
    std::string toString() const;
  };

  /**
   * @brief Evaluate the where-provenance circuit at gate @p g.
   *
   * Returns one set of @c Locator values per output column position.
   * Each @c Locator in the set identifies a base-relation cell that
   * contributed the value at that output position.
   *
   * @param g  Root gate to evaluate.
   * @return   Vector (indexed by output column) of sets of @c Locator values.
   */
  std::vector<std::set<Locator>> evaluate(gate_t g) const;
};

#endif /* WHERE_CIRCUIT_H */
