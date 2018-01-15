#ifndef WHERE_CIRCUIT_H
#define WHERE_CIRCUIT_H

#include <set>
#include <vector>
#include <utility>

#include "Circuit.hpp"

enum class WhereGate { UNDETERMINED, TIMES, PLUS, EQ, PROJECT, IN };

class WhereCircuit : public Circuit<WhereGate> {
 private:
  std::unordered_map<unsigned, uuid> input_token;
  std::unordered_map<unsigned, std::pair<std::string,int>> input_info;
  std::unordered_map<unsigned, std::vector<int>> projection_info;
  std::unordered_map<unsigned, std::pair<int,int>> equality_info;
  
 public:
  unsigned setGate(const uuid &u, WhereGate t) override;
  unsigned setGateInput(const uuid &u, std::string table, int nb_columns);
  unsigned setGateProjection(const uuid &u, std::vector<int> &&infos);
  unsigned setGateEquality(const uuid &u, int pos1, int pos2);

  std::string toString(unsigned g) const override;

  struct Locator {
    std::string table;
    uuid tid;
    int position;

    Locator(std::string t, uuid u, int i) : table(t), tid(u), position(i) {}
    bool operator<(Locator that) const;
    std::string toString() const;
  };

  std::vector<std::set<Locator>> evaluate(unsigned g) const;
};

#endif /* WHERE_CIRCUIT_H */
