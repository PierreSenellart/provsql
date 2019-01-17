#ifndef DOT_CIRCUIT_H
#define DOT_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "Circuit.hpp"

enum class DotGate { UNDETERMINED, OTIMES, OPLUS, OMINUS, \
  OMINUSR, OMINUSL, PROJECT, EQ, IN };

class DotCircuit : public Circuit<DotGate> {
 private:
  std::set<unsigned> inputs;
  std::vector<std::string> desc;
  
 public:
  unsigned addGate() override;
  unsigned setGate(const uuid &u, DotGate t) override;
  unsigned setGate(const uuid &u, DotGate t, std::string d);

  void render() const;
  
  virtual std::string toString(unsigned g) const override;
};

#endif /* DOT_CIRCUIT_H */
