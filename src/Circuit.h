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
 * - Each gate may optionally be associated with a UUID, enabling
 *   round-tripping between the in-memory circuit and the persistent mmap
 *   representation.  The UUID is kept as its 16 bytes (@c gate_uuid);
 *   the few gates built in memory under another name keep it as a string.
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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <set>
#include <utility>
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
 * @brief A UUID as its 16 bytes, read as two big-endian words.
 *
 * The key under which a circuit finds a gate from its UUID: hashing and
 * comparing it touches 16 bytes, where the text form of a UUID has 36.
 * Keys compare as their bytes do, in order (as @c memcmp would).
 */
struct gate_uuid {
  std::uint64_t hi = 0; ///< The first eight bytes, big-endian
  std::uint64_t lo = 0; ///< The last eight bytes, big-endian

  /** @brief The UUID whose 16 bytes start at @p p. */
  static gate_uuid fromBytes(const unsigned char *p) {
    gate_uuid u;
    for(int i = 0; i < 8; ++i) {
      u.hi = (u.hi << 8) | p[i];
      u.lo = (u.lo << 8) | p[i + 8];
    }
    return u;
  }

  /** @brief Write the 16 bytes of the UUID to @p p. */
  void toBytes(unsigned char *p) const {
    for(int i = 0; i < 8; ++i) {
      p[i] = static_cast<unsigned char>(hi >> (56 - 8 * i));
      p[i + 8] = static_cast<unsigned char>(lo >> (56 - 8 * i));
    }
  }

  /**
   * @brief Read @p s as a UUID in the text form PostgreSQL prints
   *        (lowercase, @c xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).
   *
   * Any other string, even one PostgreSQL would read as the same UUID,
   * is refused, so that the text of a name is given back as it came.
   *
   * @return @c false if @p s is not in that form.
   */
  static bool parse(const std::string &s, gate_uuid &u) {
    if(s.size() != 36)
      return false;
    std::uint64_t w[2] = {0, 0};
    int n = 0;
    for(std::size_t i = 0; i < 36; ++i) {
      const char c = s[i];
      if(i == 8 || i == 13 || i == 18 || i == 23) {
        if(c != '-')
          return false;
        continue;
      }
      unsigned d;
      if(c >= '0' && c <= '9')
        d = c - '0';
      else if(c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else
        return false;
      w[n / 16] = (w[n / 16] << 4) | d;
      ++n;
    }
    u.hi = w[0];
    u.lo = w[1];
    return true;
  }

  /** @brief The text form of the UUID, as PostgreSQL prints it. */
  std::string str() const {
    static const char digits[] = "0123456789abcdef";
    std::string s(36, '-');
    std::size_t pos = 0;
    for(int n = 0; n < 32; ++n) {
      if(pos == 8 || pos == 13 || pos == 18 || pos == 23)
        ++pos;
      const std::uint64_t w = n < 16 ? hi : lo;
      s[pos++] = digits[(w >> (60 - 4 * (n % 16))) & 0xF];
    }
    return s;
  }

  /** @brief Equality of the 16 bytes. */
  bool operator==(const gate_uuid &o) const {
    return hi == o.hi && lo == o.lo;
  }
  /** @brief Inequality of the 16 bytes. */
  bool operator!=(const gate_uuid &o) const {
    return !(*this == o);
  }
  /** @brief The order of the bytes. */
  bool operator<(const gate_uuid &o) const {
    return hi != o.hi ? hi < o.hi : lo < o.lo;
  }

  /** @brief Boost serialisation. */
  template<class Archive> void serialize(Archive &ar, const unsigned int) {
    ar & hi & lo;
  }
};

/** @brief Hash of a @c gate_uuid: its bytes are already uniformly spread. */
struct gate_uuid_hash {
  /** @brief The hash of @p u. */
  std::size_t operator()(const gate_uuid &u) const {
    return static_cast<std::size_t>(u.hi ^ (u.lo * 0x9E3779B97F4A7C15ULL));
  }
};

/**
 * @brief Generic template base class for provenance circuits.
 *
 * A @c Circuit stores:
 * - A vector of gate types indexed by @c gate_t.
 * - A vector of child-wire lists (also indexed by @c gate_t).
 * - Maps between gate UUIDs and @c gate_t IDs.
 *
 * @tparam gateType  Enumeration of gate kinds for this circuit variant.
 */
template<class gateType>
class Circuit {
public:
/** @brief UUID type used in this circuit (always @c std::string). */
using uuid = std::string;

protected:
/** @brief What a gate is named by, in @c name_kind. */
enum : unsigned char {
  NO_NAME,    ///< No name
  UUID_NAME,  ///< A UUID, in @c id2uuid
  OTHER_NAME  ///< Another string, in @c id2name
};

/* Each name is the name of one gate, but a gate redirected to another
 * (see @c pointName) leaves its name to it: a name -> gate map may send
 * two names to one gate, which keeps its own name. */
std::unordered_map<gate_uuid, gate_t, gate_uuid_hash> uuid2id;  ///< UUID → gate index
std::vector<gate_uuid> id2uuid;          ///< UUID of each gate (if @c UUID_NAME)
std::vector<unsigned char> name_kind;    ///< What each gate is named by
std::unordered_map<std::string, gate_t> name2id;  ///< Other name → gate index
std::unordered_map<gate_t, std::string> id2name;  ///< Gate index → other name

/** @brief What gate @p g is named by. */
unsigned char nameKind(gate_t g) const {
  const auto i = static_cast<std::underlying_type<gate_t>::type>(g);
  return i < name_kind.size() ? name_kind[i] : NO_NAME;
}

/** @brief Name gate @p g by the UUID @p u. */
void bindName(gate_t g, const gate_uuid &u);

/** @brief Name gate @p g by @p u, a UUID in its text form or another string. */
void bindName(gate_t g, const uuid &u);

/** @brief Make the name of gate @p g, if any, name gate @p target. */
void pointName(gate_t g, gate_t target);

/** @brief Remove the name of gate @p g, if any. */
void dropName(gate_t g);

/** @brief Give the name of gate @p from, if any, to gate @p to. */
void moveName(gate_t from, gate_t to);

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

/** @copydoc getGate(const uuid &) */
gate_t getGate(const gate_uuid &u) {
  return findOrAddGate(u).first;
}

/**
 * @brief Return (or create) the gate associated with UUID @p u, and
 *        whether it was created.
 */
std::pair<gate_t, bool> findOrAddGate(const gate_uuid &u);

/**
 * @brief Return the UUID string associated with gate @p g.
 * @param g  Gate identifier.
 * @return   UUID string, or empty if @p g has no UUID.
 */
uuid getUUID(gate_t g) const;

/**
 * @brief Read the UUID of gate @p g into @p u.
 * @return @c false if @p g is not named by a UUID.
 */
bool getUUID(gate_t g, gate_uuid &u) const {
  if(nameKind(g) != UUID_NAME)
    return false;
  u = id2uuid[static_cast<std::underlying_type<gate_t>::type>(g)];
  return true;
}

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

/** @copydoc hasGate(const uuid &) const */
bool hasGate(const gate_uuid &u) const {
  return uuid2id.find(u) != uuid2id.end();
}

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
 * @brief A refusal by design, or a known gap, rather than an error.
 *
 * Reported to the client as a ProvSQL refusal (SQLSTATE @c 0A000, with a
 * @c provsql-reason tag and its scope; see @c provsql_unsupported), so
 * that tooling can tell it from an internal fault.
 */
class CircuitRefusal : public CircuitException
{
const char *scope_; ///< @c PROVSQL_DELIBERATE, @c PROVSQL_GAP, ...
const char *tag_;   ///< Stable short tag of the refusal

public:
/**
 * @brief Construct a refusal.
 * @param scope  Its kind (@c PROVSQL_DELIBERATE, @c PROVSQL_GAP, ...).
 * @param tag    Its stable short tag.
 * @param m      The message a user reads.
 */
CircuitRefusal(const char *scope, const char *tag, const std::string &m)
  : CircuitException(m), scope_(scope), tag_(tag) {
}
/** @brief The kind of the refusal. */
const char *scope() const noexcept {
  return scope_;
}
/** @brief The tag of the refusal. */
const char *tag() const noexcept {
  return tag_;
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
