/**
 * @file Circuit.h
 * @brief Generic directed-acyclic-graph circuit template and gate identifier.
 *
 * This header provides two central abstractions:
 *
 * ### @c gate_t
 * A strongly-typed wrapper around @c size_t used as a compact gate
 * identifier within a circuit.  Using a distinct type prevents accidental
 * mixing of gate IDs with plain integers.  Helper operators (increment,
 * comparison, stream I/O, @c to_string) make it convenient to use in
 * loops and containers.
 *
 * ### @c Circuit<gateType>
 * A template base class parameterised by gate type, inherited by all
 * circuit variants in ProvSQL (@c BooleanCircuit, @c GenericCircuit,
 * @c DotCircuit, @c WhereCircuit).
 * A circuit is a directed acyclic graph where:
 * - Each **gate** has a type drawn from @c gateType (a user-supplied enum).
 * - **Wires** are directed edges from parent gates to their children.
 * - Each gate may optionally be associated with a UUID string, enabling
 *   round-tripping between the in-memory circuit and the persistent mmap
 *   representation.
 *
 * @c Circuit.hpp (included by subclass headers) provides the out-of-line
 * template method implementations.
 *
 * ### @c CircuitException
 * Exception type thrown when a circuit operation fails (e.g. UUID not
 * found, type mismatch).
 */
#ifndef CIRCUIT_H
#define CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <set>
#include <vector>
#include <type_traits>

/**
 * @brief Strongly-typed gate identifier.
 *
 * Wraps a @c size_t so that gate IDs are not accidentally used as plain
 * integers.  The underlying value is a zero-based index into the circuit's
 * gate and wire vectors.
 */
enum class gate_t : size_t {};

/**
 * @brief Generic template base class for provenance circuits.
 *
 * A @c Circuit stores:
 * - A vector of gate types indexed by @c gate_t.
 * - A vector of child-wire lists (also indexed by @c gate_t).
 * - Bidirectional maps between gate UUIDs (strings) and @c gate_t IDs.
 *
 * @tparam gateType  Enumeration of gate kinds for this circuit variant.
 */
template<class gateType>
class Circuit {
public:
/** @brief UUID type used in this circuit (always @c std::string). */
using uuid = std::string;

protected:
std::unordered_map<uuid, gate_t> uuid2id;  ///< UUID string → gate index
std::unordered_map<gate_t, uuid> id2uuid;  ///< Gate index → UUID string

std::vector<gateType> gates;                 ///< Gate type for each gate
std::vector<std::vector<gate_t> > wires;     ///< Child wire lists for each gate

/**
 * @brief Update the type of an existing gate.
 * @param g  Gate to update.
 * @param t  New gate type.
 */
void setGateType(gate_t g, gateType t)
{
  gates[static_cast<std::underlying_type<gate_t>::type>(g)]=t;
}

protected:
/**
 * @brief Allocate a new gate with a default-initialised type.
 *
 * Derived classes override this to perform additional initialisation
 * (e.g. resizing auxiliary vectors).
 *
 * @return The @c gate_t identifier of the newly created gate.
 */
virtual gate_t addGate();

public:
virtual ~Circuit() {
}

/**
 * @brief Return the total number of gates in the circuit.
 * @return Number of gates.
 */
std::vector<gate_t>::size_type getNbGates() const {
  return gates.size();
}

/**
 * @brief Return (or create) the gate associated with UUID @p u.
 *
 * If no gate with this UUID exists yet a new gate is allocated via
 * @c addGate() and the UUID mapping is recorded.
 *
 * @param u  UUID string.
 * @return   Gate identifier for @p u.
 */
gate_t getGate(const uuid &u);

/**
 * @brief Return the UUID string associated with gate @p g.
 * @param g  Gate identifier.
 * @return   UUID string, or empty if @p g has no UUID.
 */
uuid getUUID(gate_t g) const;

/**
 * @brief Return the type of gate @p g.
 * @param g  Gate identifier.
 * @return   The gate's type.
 */
gateType getGateType(gate_t g) const
{
  return gates[static_cast<std::underlying_type<gate_t>::type>(g)];
}

/**
 * @brief Return a mutable reference to the child-wire list of gate @p g.
 * @param g  Gate identifier.
 * @return   Reference to the vector of child gate IDs.
 */
std::vector<gate_t> &getWires(gate_t g)
{
  return wires[static_cast<std::underlying_type<gate_t>::type>(g)];
}

/**
 * @brief Return a const reference to the child-wire list of gate @p g.
 * @param g  Gate identifier.
 * @return   Const reference to the vector of child gate IDs.
 */
const std::vector<gate_t> &getWires(gate_t g) const
{
  return wires[static_cast<std::underlying_type<gate_t>::type>(g)];
}

/**
 * @brief Create or update the gate associated with UUID @p u.
 *
 * If the UUID is already mapped the existing gate's type is updated.
 * Otherwise a new gate is allocated.
 *
 * @param u     UUID string to associate with the gate.
 * @param type  Gate type.
 * @return      Gate identifier.
 */
virtual gate_t setGate(const uuid &u, gateType type);

/**
 * @brief Allocate a new gate with type @p type and no UUID.
 * @param type  Gate type.
 * @return      Gate identifier.
 */
virtual gate_t setGate(gateType type);

/**
 * @brief Test whether a gate with UUID @p u exists.
 * @param u  UUID string.
 * @return   @c true if a gate with this UUID is present.
 */
bool hasGate(const uuid &u) const;

/**
 * @brief Add a directed wire from gate @p f (parent) to gate @p t (child).
 * @param f  Source (parent) gate.
 * @param t  Target (child) gate.
 */
void addWire(gate_t f, gate_t t);

/**
 * @brief Return a textual description of gate @p g for debugging.
 *
 * Pure virtual; each concrete circuit class provides its own formatting.
 *
 * @param g  Gate to describe.
 * @return   Human-readable string.
 */
virtual std::string toString(gate_t g) const = 0;
};

/**
 * @brief Exception type thrown by circuit operations on invalid input.
 *
 * Thrown when, for example, a UUID is not found in the circuit or a
 * type constraint is violated.
 */
class CircuitException : public std::exception
{
std::string message; ///< Human-readable description of the error

public:
/**
 * @brief Construct with a descriptive error message.
 * @param m  Error message string.
 */
CircuitException(const std::string &m) : message(m) {
}
/**
 * @brief Return the error message as a C-string.
 * @return Null-terminated error description.
 */
virtual char const * what() const noexcept {
  return message.c_str();
}
};

/**
 * @brief Pre-increment operator for @c gate_t.
 * @param g  Gate to increment.
 * @return   Reference to the incremented gate.
 */
inline gate_t &operator++(gate_t &g) {
  return g=gate_t{static_cast<std::underlying_type<gate_t>::type>(g)+1};
}

/**
 * @brief Compare a @c gate_t against a @c std::vector size type.
 * @param t  Gate identifier.
 * @param u  Vector size to compare against.
 * @return   @c true if the underlying integer of @p t is less than @p u.
 */
inline bool operator<(gate_t t, std::vector<gate_t>::size_type u)
{
  return static_cast<std::underlying_type<gate_t>::type>(t)<u;
}

/**
 * @brief Convert a @c gate_t to its decimal string representation.
 * @param g  Gate identifier.
 * @return   Decimal string of the underlying integer.
 */
inline std::string to_string(gate_t g) {
  return std::to_string(static_cast<std::underlying_type<gate_t>::type>(g));
}

/**
 * @brief Read a @c gate_t from an input stream.
 * @param i  Input stream.
 * @param g  Gate to populate.
 * @return   Reference to @p i.
 */
inline std::istream &operator>>(std::istream &i, gate_t &g)
{
  std::underlying_type<gate_t>::type u;
  i >> u;
  g=gate_t{u};
  return i;
}

/**
 * @brief Write a @c gate_t to an output stream as its decimal value.
 * @param o  Output stream.
 * @param g  Gate identifier to write.
 * @return   Reference to @p o.
 */
inline std::ostream &operator<<(std::ostream &o, gate_t g)
{
  return o << static_cast<std::underlying_type<gate_t>::type>(g);
}

#endif /* CIRCUIT_H */
