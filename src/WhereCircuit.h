#ifndef WHERE_CIRCUIT_H
#define WHERE_CIRCUIT_H

#include <unordered_set>
#include <vector>
#include <utility>

#include "Circuit.hpp"

enum class WhereGate { UNDETERMINED, TIMES, PLUS, EQ, PROJECT, IN };

class WhereCircuit : public Circuit<WhereGate> {
 private:
  std::unordered_set<unsigned> inputs;
  std::unordered_map<unsigned, std::vector<int>> projection_info;
  std::unordered_map<unsigned, std::pair<int,int>> equality_info;
  
 public:
  unsigned setGate(const uuid &u, WhereGate t) override;
  unsigned setGateProjection(const uuid &u, std::vector<int> &&infos);
  unsigned setGateEquality(const uuid &u, int pos1, int pos2);

  virtual std::string toString(unsigned g) const;
};

#endif /* WHERE_CIRCUIT_H */
