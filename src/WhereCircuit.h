#ifndef WHERE_CIRCUIT_H
#define WHERE_CIRCUIT_H

#include <set>
#include <vector>
#include <utility>

#include "Circuit.hpp"

enum class WhereGate { UNDETERMINED, TIMES, PLUS, EQ, PROJECT, IN };

class WhereCircuit : public Circuit<WhereGate> {
 private:
  std::unordered_map<gate_t, uuid> input_token;
  std::unordered_map<gate_t, std::pair<std::string,int>> input_info;
  std::unordered_map<gate_t, std::vector<int>> projection_info;
  std::unordered_map<gate_t, std::pair<int,int>> equality_info;
  
 public:
  gate_t setGate(const uuid &u, WhereGate t) override;
  gate_t setGateInput(const uuid &u, std::string table, int nb_columns);
  gate_t setGateProjection(const uuid &u, std::vector<int> &&infos);
  gate_t setGateEquality(const uuid &u, int pos1, int pos2);

  std::string toString(gate_t g) const override;

  struct Locator {
    std::string table;
    uuid tid;
    int position;

    Locator(std::string t, uuid u, int i) : table(t), tid(u), position(i) {}
    bool operator<(Locator that) const;
    std::string toString() const;
  };

  std::vector<std::set<Locator>> evaluate(gate_t g) const;
};

#endif /* WHERE_CIRCUIT_H */
