#ifndef CIRCUIT_H
#define CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <vector>

template<class gateType>
class Circuit {
 public:
  using uuid = std::string;

 private:
  std::unordered_map<uuid, unsigned> uuid2id;
  
 protected:
  std::vector<gateType> gates;
  std::vector<std::vector<unsigned>> wires;
    
 public:
  virtual unsigned addGate();
  virtual unsigned setGate(const uuid &u, gateType t);
  bool hasGate(const uuid &u) const;
  unsigned getGate(const uuid &u);
  void addWire(unsigned f, unsigned t);

  virtual std::string toString(unsigned g) const = 0;
};

class CircuitException : public std::exception
{
  std::string message;

 public: 
  CircuitException(const std::string &m) : message(m) {}
  virtual char const * what() const noexcept { return message.c_str(); }
};

#endif /* CIRCUIT_H */
