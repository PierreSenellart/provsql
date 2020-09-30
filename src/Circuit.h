#ifndef CIRCUIT_H
#define CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <set>
#include <vector>
#include <type_traits>
  
enum class gate_t : size_t {};

template<class gateType>
class Circuit {
 public:
  using uuid = std::string;

 protected:
  std::unordered_map<uuid, gate_t> uuid2id;

  std::vector<gateType> gates;
  std::vector<std::vector<gate_t>> wires;

 protected:
  virtual gate_t addGate();
    
 public:
  std::vector<gate_t>::size_type getNbGates() const { return gates.size(); }
  gate_t getGate(const uuid &u);
  gateType getGateType(gate_t g) const
    { return gates[static_cast<std::underlying_type<gate_t>::type>(g)]; }
  std::vector<gate_t> &getWires(gate_t g)
    { return wires[static_cast<std::underlying_type<gate_t>::type>(g)]; }
  const std::vector<gate_t> &getWires(gate_t g) const
    { return wires[static_cast<std::underlying_type<gate_t>::type>(g)]; }

  virtual gate_t setGate(const uuid &u, gateType t);
  virtual gate_t setGate(gateType t);
  bool hasGate(const uuid &u) const;
  void addWire(gate_t f, gate_t t);

  virtual std::string toString(gate_t g) const = 0;
};

class CircuitException : public std::exception
{
  std::string message;

 public: 
  CircuitException(const std::string &m) : message(m) {}
  virtual char const * what() const noexcept { return message.c_str(); }
};

inline gate_t &operator++(gate_t &g) {
  return g=gate_t{static_cast<std::underlying_type<gate_t>::type>(g)+1};
}

inline bool operator<(gate_t t, std::vector<gate_t>::size_type u)
{
  return static_cast<std::underlying_type<gate_t>::type>(t)<u;
}

inline std::string to_string(gate_t g) {
  return std::to_string(static_cast<std::underlying_type<gate_t>::type>(g));
}

inline std::istream &operator>>(std::istream &i, gate_t &g)
{
  std::underlying_type<gate_t>::type u;
  i >> u;
  g=gate_t{u};
  return i;
}

inline std::ostream &operator<<(std::ostream &o, gate_t g)
{
  return o << static_cast<std::underlying_type<gate_t>::type>(g);
}

#endif /* CIRCUIT_H */
