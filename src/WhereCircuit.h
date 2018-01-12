#ifndef WHERE_CIRCUIT_H
#define WHERE_CIRCUIT_H

#include <unordered_set>
#include <vector>

#include "Circuit.hpp"

enum class WhereGate { UNDETERMINED, TIMES, PLUS, EQ, PROJECT, IN };

class WhereCircuit : public Circuit<WhereGate> {
 private:
  std::unordered_set<unsigned> inputs;
  
 public:
  unsigned addGate() override;
  unsigned setGate(const uuid &u, WhereGate t) override;
  unsigned setGate(const uuid &u, WhereGate t, int info1, int info2 = -1);

  virtual std::string toString(unsigned g) const;
};

#endif /* WHERE_CIRCUIT_H */
