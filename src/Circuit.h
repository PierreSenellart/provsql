#ifndef CIRCUIT_H
#define CIRCUIT_H

extern "C" {
#include "utils/uuid.h"
}  

#include <unordered_map>
#include <unordered_set>
#include <vector>

class Circuit {
 public:
  enum gateType { UNDETERMINED, AND, OR, NOT, IN };
  typedef std::string uuid;

 private:
  std::vector<gateType> gates;
  std::vector<std::unordered_set<unsigned>> wires;
  std::vector<std::unordered_set<unsigned>> rwires;
  std::unordered_map<uuid, unsigned> uuid2id;
  std::vector<double> prob;
  std::unordered_set<unsigned> inputs;
    
 public:
  bool hasGate(const uuid &u) const;
  unsigned getGate(const uuid &u);
  unsigned addGate(gateType type);
  void setGate(const uuid &u, gateType t, double p = -1);
  void addWire(unsigned f, unsigned t);

  bool evaluate(unsigned g, const std::unordered_set<unsigned> &trues) const;
  double monteCarlo(unsigned g, unsigned samples) const;

  std::string toString(unsigned g) const;
};

#endif /* CIRCUIT_H */
